#pragma once
// RMTC 参赛程序：感知解析 + 地图记忆 + 决策（全部头文件内实现）
//
// 设计要点（对应规则与协议）：
//  · 只信 /team_X/perception，动作只发 /team_X/action，tick 与 session_id 原样回填
//  · 视野只有 ±4 格的 9x9，且每个可见实体自带一张 9x9 字符串（中心固定第 5 行第 5 列）
//  · 障碍/资源看到了就记进 World，敌人离开视野就靠记忆和推断，不假设实时真值
//  · 第一阶段只有 MOVE/WAIT；没有看到任何敌车时一律不发 ATTACK（安全且合规）
//  · 所有异常一律降级为 WAIT，绝不因为解析失败而崩溃或乱动

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rmtc {

using json = nlohmann::json;
using Pos = std::pair<int, int>;  // (x, y)，x 向右、y 向下，左上角 (0,0)

// ---------------------------------------------------------------- 安全取值
inline int jint(const json& j, const char* k, int def = 0) {
  auto it = j.find(k);
  if (it == j.end() || it->is_null()) return def;
  if (it->is_number_integer()) return it->get<int>();
  if (it->is_number()) return static_cast<int>(it->get<double>());
  if (it->is_string()) {
    try { return std::stoi(it->get<std::string>()); } catch (...) { return def; }
  }
  return def;
}

inline double jdbl(const json& j, const char* k, double def = 0.0) {
  auto it = j.find(k);
  if (it == j.end() || it->is_null()) return def;
  if (it->is_number()) return it->get<double>();
  if (it->is_string()) {
    try { return std::stod(it->get<std::string>()); } catch (...) { return def; }
  }
  return def;
}

inline bool jbool(const json& j, const char* k, bool def = false) {
  auto it = j.find(k);
  if (it == j.end() || it->is_null()) return def;
  if (it->is_boolean()) return it->get<bool>();
  if (it->is_number()) return it->get<double>() != 0.0;
  return def;
}

inline std::string jstr(const json& j, const char* k, const std::string& def = "") {
  auto it = j.find(k);
  if (it == j.end() || it->is_null()) return def;
  if (it->is_string()) return it->get<std::string>();
  return def;
}

// ---------------------------------------------------------------- 感知结构
struct Visible {
  std::string type;  // robot / resource_normal / resource_high / obstacle
  std::string id, team, robot_id;
  int x = -1, y = -1;   // 已还原成绝对坐标
  int hp = 100;
  int value = 0;
  bool defending = false;
  bool protected_ = false;
};

struct Mission {
  std::string id, type;
  int target = 0;
  int x = -1, y = -1;
};

struct Perception {
  int tick = -1;
  std::string session_id;
  int vision_radius = 4;

  int self_x = -1, self_y = -1;
  std::string position_reason;  // initial / respawn / 空
  int hp = 100;
  bool alive = true;
  bool defending = false;
  bool protected_ = false;
  int respawn_in = 0;
  std::string last_action, last_error;
  bool last_moved = false;

  double effective_damage = 0.0;
  double resource_value = 0.0;
  int mission_completed = 0;
  std::vector<std::string> mission_completed_ids;
  int invalid_actions = 0;

  std::vector<Visible> entities;
  std::vector<Mission> missions;
  std::string mission_text, event_text;
  int map_w = 24, map_h = 24;

  bool seen_enemy = false;

  // 9x9 相对位置图 -> 绝对坐标。返回 false 表示图里没有 '2'（说明与自身重叠）
  static bool locate(const std::string& grid, int sx, int sy, int& ox, int& oy) {
    int row = 0, col = 0;
    for (char ch : grid) {
      if (ch == '\n') { ++row; col = 0; continue; }
      if (ch == '\r') continue;
      if (ch == '2') {
        ox = sx + (col - 4);  // 中心固定在第 5 行第 5 列（列/行从 1 开始）
        oy = sy + (row - 4);
        return true;
      }
      ++col;
    }
    ox = sx;
    oy = sy;
    return false;
  }
};

// 解析感知 JSON。失败返回 nullopt，调用方降级 WAIT。
inline std::optional<Perception> parse_perception(const std::string& raw) {
  json j = json::parse(raw, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;

  Perception p;
  p.tick = jint(j, "tick", -1);
  p.session_id = jstr(j, "session_id");
  p.vision_radius = jint(j, "vision_radius", 4);
  p.mission_text = jstr(j, "mission_text");
  p.event_text = jstr(j, "event_text");

  if (p.tick < 0 || p.session_id.empty()) return std::nullopt;

  auto self = j.find("self");
  if (self != j.end() && self->is_object()) {
    auto pos = self->find("position");
    if (pos != self->end() && pos->is_array() && pos->size() >= 2) {
      p.self_x = (*pos)[0].get<int>();
      p.self_y = (*pos)[1].get<int>();
    }
    p.position_reason = jstr(*self, "position_reason");
    p.hp = jint(*self, "hp", 100);
    p.alive = jbool(*self, "alive", true);
    p.defending = jbool(*self, "defending", false);
    p.protected_ = jbool(*self, "protected", false);
    p.respawn_in = jint(*self, "respawn_in", 0);
    auto la = self->find("last_action");
    if (la != self->end() && la->is_object()) {
      p.last_action = jstr(*la, "action");
      p.last_moved = jbool(*la, "moved", false);
      p.last_error = jstr(*la, "error");
    }
  }
  if (p.self_x < 0 || p.self_y < 0) return std::nullopt;

  auto sc = j.find("score");
  if (sc != j.end() && sc->is_object()) {
    p.effective_damage = jdbl(*sc, "effective_damage", 0.0);
    p.resource_value = jdbl(*sc, "resource_value", 0.0);
    p.mission_completed = jint(*sc, "mission_completed", 0);
    p.invalid_actions = jint(*sc, "invalid_actions", 0);
    auto ids = sc->find("mission_completed_ids");
    if (ids != sc->end() && ids->is_array())
      for (auto& v : *ids)
        if (v.is_string()) p.mission_completed_ids.push_back(v.get<std::string>());
  }

  auto mp = j.find("map");
  if (mp != j.end() && mp->is_object()) {
    p.map_w = jint(*mp, "width", 24);
    p.map_h = jint(*mp, "height", 24);
  }

  auto ents = j.find("visible_entities");
  if (ents != j.end() && ents->is_array()) {
    for (auto& e : *ents) {
      if (!e.is_object()) continue;
      Visible v;
      v.type = jstr(e, "type");
      v.id = jstr(e, "id");
      v.team = jstr(e, "team");
      v.robot_id = jstr(e, "robot_id");
      v.hp = jint(e, "hp", 100);
      v.value = jint(e, "value", 0);
      v.defending = jbool(e, "defending", false);
      v.protected_ = jbool(e, "protected", false);
      if (v.type == "robot") p.seen_enemy = true;
      std::string grid = jstr(e, "position");
      if (!grid.empty()) {
        Perception::locate(grid, p.self_x, p.self_y, v.x, v.y);
      } else {
        v.x = p.self_x;
        v.y = p.self_y;
      }
      p.entities.push_back(v);
    }
  }

  auto ms = j.find("missions");
  if (ms != j.end() && ms->is_array()) {
    for (auto& m : *ms) {
      if (!m.is_object()) continue;
      Mission mm;
      mm.id = jstr(m, "id");
      mm.type = jstr(m, "type");
      mm.target = jint(m, "target", 0);
      auto pos = m.find("position");
      if (pos != m.end() && pos->is_array() && pos->size() >= 2) {
        mm.x = (*pos)[0].get<int>();
        mm.y = (*pos)[1].get<int>();
      }
      p.missions.push_back(mm);
    }
  }
  return p;
}

// ---------------------------------------------------------------- 地图记忆
enum : uint8_t { C_UNKNOWN = 0, C_FREE = 1, C_WALL = 2 };

struct World {
  int w = 24, h = 24;
  std::vector<uint8_t> cell;
  std::map<Pos, int> res;  // 资源位置 -> 价值（10/20）
  // 从"移动失败回执"学到的临时禁行格：值 = 到期 tick。
  // 没有它，程序会一直往同一个撞不动的格子走（日志里出现过卡 20 Tick）。
  std::map<Pos, int> temp_block;

  void init(int width, int height) {
    if (width == w && height == h && !cell.empty()) return;
    w = std::max(1, width);
    h = std::max(1, height);
    cell.assign(static_cast<size_t>(w) * h, C_UNKNOWN);
    res.clear();
    temp_block.clear();
  }

  void mark_temp_block(Pos q, int until_tick) { temp_block[q] = until_tick; }

  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  bool walkable(int x, int y) const {
    return in(x, y) && cell[static_cast<size_t>(y) * w + x] != C_WALL;
  }
  void mark(int x, int y, uint8_t v) {
    if (in(x, y)) cell[static_cast<size_t>(y) * w + x] = v;
  }

  // 用当前视野更新记忆：障碍记住、资源记住、资源在视野内消失即视为已被采走
  void observe(const Perception& p) {
    mark(p.self_x, p.self_y, C_FREE);
    // 阵亡时 visible_entities 为空；若继续执行下面"视野内没看到就当已被采走"的逻辑，
    // 会把死亡点附近记过的资源全部误删，复活后就成了瞎子。所以阵亡帧只更新自身格。
    if (!p.alive) return;
    std::map<Pos, int> seen_res;
    for (const auto& e : p.entities) {
      if (!in(e.x, e.y)) continue;
      if (e.type == "obstacle") {
        mark(e.x, e.y, C_WALL);
      } else if (e.type == "resource_normal" || e.type == "resource_high") {
        mark(e.x, e.y, C_FREE);
        res[{e.x, e.y}] = (e.value > 0 ? e.value : (e.type == "resource_high" ? 20 : 10));
        seen_res[{e.x, e.y}] = 1;
      } else {
        mark(e.x, e.y, C_FREE);  // 敌车占位不能进，但格子本身是可通行的
      }
    }
    // 视野内、之前记过、这次没看到 -> 已被采走（资源不会自己移动）
    const int r = p.vision_radius;
    std::vector<Pos> gone;
    for (const auto& kv : res) {
      const Pos& q = kv.first;
      if (std::abs(q.first - p.self_x) > r || std::abs(q.second - p.self_y) > r) continue;
      if (seen_res.count(q) == 0) gone.push_back(q);
    }
    for (const auto& q : gone) res.erase(q);
  }

  std::optional<Pos> nearest_resource(Pos from, const std::vector<Pos>& avoid) const {
    std::optional<Pos> best;
    int best_d = 1 << 30;
    for (const auto& kv : res) {
      const Pos& q = kv.first;
      if (std::find(avoid.begin(), avoid.end(), q) != avoid.end()) continue;
      int d = std::abs(q.first - from.first) + std::abs(q.second - from.second);
      if (d < best_d) { best_d = d; best = q; }
    }
    return best;
  }

  std::vector<Pos> unknown_cells(int limit = 400) const {
    std::vector<Pos> out;
    for (int y = 0; y < h && static_cast<int>(out.size()) < limit; ++y)
      for (int x = 0; x < w && static_cast<int>(out.size()) < limit; ++x)
        if (cell[static_cast<size_t>(y) * w + x] == C_UNKNOWN) out.push_back({x, y});
    return out;
  }

  // BFS：返回从 from 迈出的第一步；到不了返回 nullopt
  // allow_unknown=false 时只走"已知可通行"的格子，用来避免盲走撞墙浪费 Tick
  std::optional<Pos> first_step(Pos from, const std::vector<Pos>& targets,
                                const std::vector<Pos>& blocked,
                                bool allow_unknown = true) const {
    if (targets.empty()) return std::nullopt;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<char> is_t(n, 0), ban(n, 0), seen(n, 0);
    for (const auto& t : targets)
      if (in(t.first, t.second)) is_t[static_cast<size_t>(t.second) * w + t.first] = 1;
    for (const auto& b : blocked)
      if (in(b.first, b.second)) ban[static_cast<size_t>(b.second) * w + b.first] = 1;

    auto passable = [&](int x, int y) {
      if (!walkable(x, y)) return false;
      if (!allow_unknown &&
          cell[static_cast<size_t>(y) * w + x] == C_UNKNOWN)
        return false;
      return true;
    };

    std::vector<Pos> step_of(n, {-1, -1});
    std::vector<Pos> q;
    q.reserve(n);
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    for (int i = 0; i < 4; ++i) {
      const int nx = from.first + dx[i], ny = from.second + dy[i];
      if (!passable(nx, ny)) continue;
      const size_t id = static_cast<size_t>(ny) * w + nx;
      if (ban[id] || seen[id]) continue;
      seen[id] = 1;
      step_of[id] = {nx, ny};
      q.push_back({nx, ny});
    }
    size_t head = 0;
    while (head < q.size()) {
      const Pos c = q[head++];
      const size_t cid = static_cast<size_t>(c.second) * w + c.first;
      if (is_t[cid]) return step_of[cid];
      for (int i = 0; i < 4; ++i) {
        const int nx = c.first + dx[i], ny = c.second + dy[i];
        if (!passable(nx, ny)) continue;
        const size_t nid = static_cast<size_t>(ny) * w + nx;
        if (ban[nid] || seen[nid]) continue;
        seen[nid] = 1;
        step_of[nid] = step_of[cid];
        q.push_back({nx, ny});
      }
    }
    return std::nullopt;
  }
};

// ---------------------------------------------------------------- LLM 结论
struct Danger {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int until_tick = 0;
};

struct Insight {
  std::vector<Danger> danger;
  int reversed_until_tick = -1;  // 幻术：MOVE 方向反转
  int target_x = -1, target_y = -1;
  std::string advice;
  int for_tick = -1;
  bool valid = false;
};

// 解析 LLM 返回的 JSON（我们自己在 prompt 里约定的结构），失败就整段丢弃
inline Insight parse_insight(const std::string& content, int tick) {
  Insight ins;
  json j = json::parse(content, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return ins;
  auto dg = j.find("danger");
  if (dg != j.end() && dg->is_array()) {
    for (auto& d : *dg) {
      if (!d.is_object()) continue;
      Danger z;
      z.x0 = jint(d, "x0", 0);
      z.y0 = jint(d, "y0", 0);
      z.x1 = jint(d, "x1", 0);
      z.y1 = jint(d, "y1", 0);
      z.until_tick = jint(d, "until_tick", 0);
      if (z.x1 >= z.x0 && z.y1 >= z.y0) ins.danger.push_back(z);
    }
  }
  ins.reversed_until_tick = jint(j, "reversed_until_tick", -1);
  auto tg = j.find("target");
  if (tg != j.end() && tg->is_array() && tg->size() >= 2) {
    ins.target_x = (*tg)[0].get<int>();
    ins.target_y = (*tg)[1].get<int>();
  }
  ins.advice = jstr(j, "advice");
  ins.for_tick = tick;
  ins.valid = true;
  return ins;
}

// ---------------------------------------------------------------- 决策
struct Decision {
  std::string action = "wait";
  std::string dir;
  std::string goal;  // reach / res / explore / flee / attack / none，仅用于日志与排错
};

inline void reverse_dir(std::string& d) {
  if (d == "up") d = "down";
  else if (d == "down") d = "up";
  else if (d == "left") d = "right";
  else if (d == "right") d = "left";
}

inline bool mission_done(const Perception& p, const std::string& id) {
  for (const auto& s : p.mission_completed_ids)
    if (s == id) return true;
  return false;
}

// 普通攻击：指定方向直线 1-2 格内最近的存活敌车；障碍不阻断，但出生保护目标会挡枪
inline bool best_attack(const Perception& p, std::string& out_dir, int* out_target_hp = nullptr) {
  if (p.protected_) return false;  // 出生保护期间不能 ATTACK
  const char* dirs[4] = {"up", "down", "left", "right"};
  const int dx[4] = {0, 0, -1, 1};
  const int dy[4] = {-1, 1, 0, 0};
  for (int i = 0; i < 4; ++i) {
    bool ray_dead = false;  // 该方向最近目标是出生保护 -> 整条射线都打不到后面的
    for (int dist = 1; dist <= 2 && !ray_dead; ++dist) {
      const int tx = p.self_x + dx[i] * dist;
      const int ty = p.self_y + dy[i] * dist;
      for (const auto& e : p.entities) {
        if (e.type != "robot") continue;
        if (e.x != tx || e.y != ty) continue;
        if (e.protected_) { ray_dead = true; break; }  // 打不动，还挡住后面
        if (out_target_hp != nullptr) *out_target_hp = e.hp;
        out_dir = dirs[i];
        return true;
      }
    }
  }
  return false;
}

inline std::optional<Pos> nearest_enemy(const Perception& p) {
  std::optional<Pos> best;
  int bd = 1 << 30;
  for (const auto& e : p.entities) {
    if (e.type != "robot") continue;
    int d = std::abs(e.x - p.self_x) + std::abs(e.y - p.self_y);
    if (d < bd) { bd = d; best = Pos{e.x, e.y}; }
  }
  return best;
}

// 远离最近敌人：优先沿"背离方向"走一格
inline std::optional<std::string> flee_dir(const Perception& p, const World& w,
                                           const std::vector<Pos>& blocked) {
  auto e = nearest_enemy(p);
  if (!e) return std::nullopt;
  const int dx = p.self_x - e->first;
  const int dy = p.self_y - e->second;
  std::vector<std::string> cand;
  if (std::abs(dx) >= std::abs(dy)) {
    cand.push_back(dx >= 0 ? "right" : "left");
    cand.push_back(dy >= 0 ? "down" : "up");
  } else {
    cand.push_back(dy >= 0 ? "down" : "up");
    cand.push_back(dx >= 0 ? "right" : "left");
  }
  cand.push_back("up");
  cand.push_back("down");
  cand.push_back("left");
  cand.push_back("right");
  for (const auto& d : cand) {
    int nx = p.self_x, ny = p.self_y;
    if (d == "up") --ny;
    else if (d == "down") ++ny;
    else if (d == "left") --nx;
    else if (d == "right") ++nx;
    if (!w.walkable(nx, ny)) continue;
    if (std::find(blocked.begin(), blocked.end(), Pos{nx, ny}) != blocked.end()) continue;
    return d;
  }
  return std::nullopt;
}

inline Decision decide(const Perception& p, World& w, const Insight& ins, int tick,
                       const std::string& mode) {
  Decision d;
  if (!p.alive) return d;            // 阵亡期间只能 WAIT
  if (p.self_x < 0 || p.self_y < 0) return d;

  // 禁行格：敌车占位 + LLM 判定的危险区
  std::vector<Pos> blocked;
  for (const auto& e : p.entities)
    if (e.type == "robot") blocked.push_back({e.x, e.y});
  // 撞不动的格子短期回避（由 last_action 回执学到）
  for (const auto& kv : w.temp_block)
    if (tick < kv.second) blocked.push_back(kv.first);
  for (const auto& z : ins.danger)
    if (tick < z.until_tick)
      for (int x = z.x0; x <= z.x1; ++x)
        for (int y = z.y0; y <= z.y1; ++y) blocked.push_back({x, y});

  // 攻击门槛：按 decision_config.json 里的策略类型调整，血量不够就先保命
  int attack_hp = 40;
  if (mode == "collect") attack_hp = 999;       // 只采集，不主动开战
  else if (mode == "defensive") attack_hp = 70; // 只在自己很健康时反击
  else if (mode == "aggressive") attack_hp = 15;

  if (p.seen_enemy) {
    std::string atk_dir;
    int target_hp = 1000;
    // 血够就打；敌车只剩 <=20（再一下就能带走）时，即使自己残血也优先收掉
    if (best_attack(p, atk_dir, &target_hp) &&
        (p.hp >= attack_hp || target_hp <= 20)) {
      d.action = "attack";
      d.dir = atk_dir;
      d.goal = "attack";
      return d;  // 攻击不受幻术影响，不反转
    }
  }

  // 残血逃离
  if (p.seen_enemy && p.hp <= 30) {
    auto fd = flee_dir(p, w, blocked);
    if (fd) { d.action = "move"; d.dir = *fd; d.goal = "flee"; }
    else { d.action = "defend"; d.goal = "defend"; }
  } else {
    // 目标优先级：结构化任务点 > LLM 给的点 > 可达的最近资源 > 未知区域探索
    // reach 任务的坐标在感知 missions[] 里就有，不依赖 AI 输出。
    const Mission* reach = nullptr;
    for (const auto& m : p.missions)
      if (m.type == "reach" && m.x >= 0 && m.y >= 0) reach = &m;

    const bool reach_done = (reach != nullptr) && mission_done(p, reach->id);
    // 按计分公式排优先级：资源 30 分需 120 价值（每点价值 0.25 分），
    // 而 reach 任务只有 5 分。所以先把资源采满，再回头去任务点。
    const bool res_full = p.resource_value >= 120.0;

    const Pos me{p.self_x, p.self_y};
    std::optional<Pos> step;
    // 两段式寻路：先只走"已知可通行"的格子（不撞墙），走不通再允许穿过未知区
    auto try_step = [&](const std::vector<Pos>& targets) -> std::optional<Pos> {
      auto s = w.first_step(me, targets, blocked, false);
      if (!s) s = w.first_step(me, targets, blocked, true);
      return s;
    };

    // 1) 任务点：资源分已经吃满，或进入后半程（防止一直采不到又放弃这 5 分）
    if (reach != nullptr && !reach_done && (res_full || p.tick >= 400)) {
      step = try_step({Pos{reach->x, reach->y}});
      if (step) d.goal = "reach";
    }
    // 2) LLM 推断的目标点（备用，mock AI 下一般拿不到）
    if (!step && ins.target_x >= 0 && ins.target_y >= 0 && !reach_done) {
      step = try_step({Pos{ins.target_x, ins.target_y}});
      if (step) d.goal = "llm";
    }
    // 3) 资源：按"距离 + 敌人威胁"排序逐个试可达性
    //    （资源被抢光/不可达时不能傻站着，也不能一直盲目探索）
    if (!step) {
      std::vector<Pos> cand;
      cand.reserve(w.res.size());
      for (const auto& kv : w.res) {
        if (std::find(blocked.begin(), blocked.end(), kv.first) != blocked.end()) continue;
        cand.push_back(kv.first);
      }
      // 敌人附近扣分：3 格内有几台敌车就罚几次。
      // 开局前 20 Tick 不罚（先抢近处资源），血量掉到 60 以下罚得更重（保命优先）。
      const int risk_w = (p.hp < 60) ? 10 : (p.tick <= 20 ? 0 : 6);
      auto risk = [&p](const Pos& q) {
        int n = 0;
        for (const auto& e : p.entities) {
          if (e.type != "robot") continue;
          if (std::abs(e.x - q.first) <= 3 && std::abs(e.y - q.second) <= 3) ++n;
        }
        return n;
      };
      // 成本 = 距离 + 威胁惩罚 - 价值奖励
      // 高价值资源（20 分）相当于"近 2 格"：否则会为了省 1 格去抢 10 分资源而放走 20 分的
      auto cost = [&me, &risk, risk_w, &w](const Pos& q) {
        int v = 10;
        const auto it = w.res.find(q);
        if (it != w.res.end()) v = it->second;
        const int bonus = (v >= 20) ? 2 : 0;
        return std::abs(q.first - me.first) + std::abs(q.second - me.second) +
               risk_w * risk(q) - bonus;
      };
      std::sort(cand.begin(), cand.end(),
                [&cost](const Pos& a, const Pos& b) { return cost(a) < cost(b); });
      for (const auto& c : cand) {
        step = try_step({c});
        if (step) { d.goal = "res"; break; }
      }
    }
    // 4) 场上已经没资源可追 -> 至少把 reach 的 5 分拿到
    if (!step && reach != nullptr && !reach_done) {
      step = try_step({Pos{reach->x, reach->y}});
      if (step) d.goal = "reach";
    }
    // 5) 猎杀：资源被抢光、任务也做完了，别浪费剩余 Tick —— 主动去找能打赢的敌车。
    //    每 20 点伤害 = 0.6 分，100 Tick 的追杀能换回不少分数。
    if (!step && p.hp > 30 && p.seen_enemy && mode != "collect") {
      int best_dist = 1 << 30;
      std::optional<Pos> best_step;
      for (const auto& e : p.entities) {
        if (e.type != "robot") continue;
        if (e.protected_) continue;                       // 打不动
        if (e.hp > 60 && p.hp < 70) continue;             // 血量没优势就不去惹满血的
        // 目标点取敌车四邻格（不能直接站到它头上）
        const std::vector<Pos> ring = {{e.x + 1, e.y}, {e.x - 1, e.y},
                                       {e.x, e.y + 1}, {e.x, e.y - 1}};
        auto s = try_step(ring);
        if (!s) continue;
        const int dist_e = std::abs(e.x - me.first) + std::abs(e.y - me.second);
        if (dist_e < best_dist) { best_dist = dist_e; best_step = s; }
      }
      if (best_step) { step = best_step; d.goal = "hunt"; }
    }

    // 6) 探索未知区域
    if (!step) {
      step = try_step(w.unknown_cells());
      if (step) d.goal = "explore";
    }

    if (step) {
      const int dx = step->first - p.self_x;
      const int dy = step->second - p.self_y;
      d.action = "move";
      if (dx > 0) d.dir = "right";
      else if (dx < 0) d.dir = "left";
      else if (dy > 0) d.dir = "down";
      else if (dy < 0) d.dir = "up";
    } else {
      d.goal = "none";
    }
  }

  // 幻术：只有 MOVE 会被反转
  if (d.action == "move" && ins.reversed_until_tick > tick) reverse_dir(d.dir);
  return d;
}

// ---------------------------------------------------------------- 工具
inline std::string action_to_json(const Perception& p, const Decision& d) {
  json j;
  j["tick"] = p.tick;              // 必须是整数，且等于所依据感知的 tick
  j["session_id"] = p.session_id;  // 必须是字符串，原样回填
  j["action"] = d.action;          // move / attack / defend / wait，必须小写
  if (d.action == "move" || d.action == "attack") j["direction"] = d.dir;
  return j.dump();
}

}  // namespace rmtc
