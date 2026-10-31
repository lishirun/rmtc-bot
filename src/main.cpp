// RMTC 参赛主程序
//
// 运行方式（联调示例，正式比赛按主办方公布的接入信息替换）：
//   export ROS_DOMAIN_ID=27
//   export ROS_LOCALHOST_ONLY=0
//   export RMTC_SEAT=A
//   export RMTC_TEAM_TOKEN=practice-A
//   export RMTC_AI_URL=http://127.0.0.1:8080/v1/chat/completions
//   ros2 run rmtc_bot contestant

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "bot.hpp"
#include "llm.hpp"

namespace {

std::string env_or(const char* k, const std::string& def) {
  const char* v = std::getenv(k);
  return (v != nullptr && v[0] != '\0') ? std::string(v) : def;
}

// 版本标记：启动日志第一行会打印它。跑的不是新程序时，第一眼就能看出来。
// 每次改代码都要改这个字符串，update-bot.sh 会校验它有没有进二进制。
constexpr const char* kBuildTag = "v12-20261031-value";

// 现场只允许改 decision_config.json 里的 decision_type
std::string load_mode() {
  const std::string path = env_or("RMTC_DECISION_CONFIG", "decision_config.json");
  std::ifstream in(path);
  if (!in) return "balanced";
  try {
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "balanced";
    return rmtc::jstr(j, "decision_type", "balanced");
  } catch (...) {
    return "balanced";
  }
}

const char* kPromptTail =
    "\n只输出 JSON，不要解释、不要代码块。格式：\n"
    "{\"danger\":[{\"x0\":0,\"y0\":0,\"x1\":0,\"y1\":0,\"until_tick\":0}],"
    "\"reversed_until_tick\":-1,\"target\":[0,0],\"advice\":\"一句话\"}\n"
    "要求：danger 只填公告中明确会造成伤害或阻挡的区域（绝对坐标，含边界），没有就给空数组；\n"
    "reversed_until_tick 只在移动方向被反转时填到期 tick，否则 -1；\n"
    "target 只在公告给出需要到达的坐标时才写，否则省略该字段；\n"
    "坐标一律是 [x, y]，x 向右、y 向下，左上角为 [0, 0]。";

class Bot : public rclcpp::Node {
 public:
  Bot() : rclcpp::Node("rmtc_contestant") {
    seat_ = env_or("RMTC_SEAT", "A");
    team_name_ = env_or("RMTC_TEAM_NAME", "队伍名称");
    mode_ = load_mode();
    debug_ = !env_or("RMTC_DEBUG", "").empty();

    const std::string ai_url =
        env_or("RMTC_AI_URL", "http://127.0.0.1:8000/v1/chat/completions");
    ai_token_ = env_or("RMTC_TEAM_TOKEN", "");
    const std::string model = env_or("RMTC_AI_MODEL", "");
    llm_ = std::make_unique<rmtc::LlmClient>(ai_url, ai_token_, model);
    llm_->set_max_calls(std::atoi(env_or("RMTC_AI_MAX_CALLS", "40").c_str()));

    auto qos_data = rclcpp::QoS(rclcpp::KeepLast(1)).reliable();
    auto qos_keep = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    // 协议 §2 表：事件公告是 RELIABLE + TRANSIENT_LOCAL + 历史深度 128
    auto qos_event = rclcpp::QoS(rclcpp::KeepLast(128)).reliable().transient_local();

    sub_perception_ = create_subscription<std_msgs::msg::String>(
        "/team_" + seat_ + "/perception", qos_data,
        [this](std_msgs::msg::String::SharedPtr msg) { on_perception(msg->data); });

    pub_action_ = create_publisher<std_msgs::msg::String>(
        "/team_" + seat_ + "/action", qos_data);
    pub_registration_ = create_publisher<std_msgs::msg::String>(
        "/team_" + seat_ + "/registration", qos_keep);

    // 事件与任务是纯文本公告；事件话题 depth 128，允许重复内容，不要当 JSON 解析
    sub_event_ = create_subscription<std_msgs::msg::String>(
        "/referee/event_text", qos_event,
        [this](std_msgs::msg::String::SharedPtr msg) { judge_event_text_ = msg->data; });
    sub_mission_ = create_subscription<std_msgs::msg::String>(
        "/referee/mission", qos_keep,
        [this](std_msgs::msg::String::SharedPtr msg) { judge_mission_text_ = msg->data; });

    reg_timer_ = create_wall_timer(std::chrono::seconds(5),
                                   [this]() { publish_registration(); });
    publish_registration();

    RCLCPP_INFO(get_logger(), "======== BUILD %s ========", kBuildTag);
    RCLCPP_INFO(get_logger(), "seat=%s mode=%s ai=%s token=%s", seat_.c_str(),
                mode_.c_str(), ai_url.c_str(),
                ai_token_.empty() ? "(未设置)" : "(已设置)");
  }

 private:
  // ---------------------------------------------------------------- 发布
  void publish_registration() {
    std_msgs::msg::String msg;
    nlohmann::json j;
    j["team"] = seat_;
    j["name"] = team_name_;
    msg.data = j.dump();
    pub_registration_->publish(msg);
  }

  // ---------------------------------------------------------------- 主循环
  // 关键防护：任何异常都不能让进程退出 —— 比赛一崩这场就废了。
  void on_perception(const std::string& raw) {
    try {
      handle_perception(raw);
    } catch (const std::exception& e) {
      RCLCPP_ERROR(get_logger(), "感知处理异常：%s", e.what());
      publish_wait();
    } catch (...) {
      RCLCPP_ERROR(get_logger(), "感知处理出现未知异常");
      publish_wait();
    }
  }

  // 兜底：异常时尽量回一条 WAIT（比什么都不发安全，裁判会按未提交处理）
  void publish_wait() {
    if (last_tick_ < 0 || session_.empty()) return;
    try {
      nlohmann::json j;
      j["tick"] = last_tick_;
      j["session_id"] = session_;
      j["action"] = "wait";
      std_msgs::msg::String out;
      out.data = j.dump();
      pub_action_->publish(out);
    } catch (...) {
      // 兜底失败也没办法了，别再抛
    }
  }

  void handle_perception(const std::string& raw) {
    auto parsed = rmtc::parse_perception(raw);
    if (!parsed) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "感知解析失败，本 Tick 不发动作");
      return;
    }
    const rmtc::Perception& p = *parsed;

    // 换场必须先处理：新场次的 tick 从 0 重新开始，
    // 若先做 tick 比较，新场次的 0..旧tick 全会被误判成迟到消息丢弃。
    if (p.session_id != session_) {
      session_ = p.session_id;
      last_tick_ = -1;
      world_ = rmtc::World();
      insight_ = rmtc::Insight();
      asked_mission_.clear();
      asked_event_.clear();
      last_ask_tick_ = -1000;
      session_calls_ = 0;
      // 手册：进入新场次后清除上场的规划、动作及过期模型结果
      last_cmd_ok_ = false;    // 上一场的出发格不能拿来标记新场地图
      llm_->drop_pending();    // 丢弃上一场遗留的 AI 结论
      RCLCPP_INFO(get_logger(), "新场次 session_id=%s（收到首帧 tick=%d）",
                  session_.c_str(), p.tick);
    }

    // 同场次内丢弃迟到消息；但 tick==0 永远接受 ——
    // 裁判在"部署新练习"时可能先发一条带旧 tick 的暂停快照，不能让它挡住真正开局的 tick=0。
    if (p.tick < last_tick_ && p.tick != 0) return;
    last_tick_ = p.tick;
    if (p.tick == 0) RCLCPP_INFO(get_logger(), "进场，开始执行策略");

    world_.init(p.map_w, p.map_h);
    world_.observe(p);
    learn_from_receipt(p);

    maybe_ask_llm(p);
    poll_llm(p.tick);

    const rmtc::Decision d = rmtc::decide(p, world_, insight_, p.tick, mode_);

    std_msgs::msg::String out;
    out.data = rmtc::action_to_json(p, d);
    pub_action_->publish(out);
    // 记下这条指令的出发格与方向，下一帧回执如果 moved=false 就能反推出哪个格子走不通
    last_cmd_x_ = p.self_x;
    last_cmd_y_ = p.self_y;
    last_cmd_dir_ = d.dir;
    last_cmd_ok_ = (d.action == "move");

    // 每 10 Tick 打一行（stage1 共 100 Tick），RMTC_DEBUG=1 时每帧都打
    // 末尾带 self.last_action 回执 —— 检录要求能证明"本程序收到并核对了执行回执"
    if (debug_ || p.tick % 10 == 0) {
      RCLCPP_INFO(get_logger(),
                  "tick=%d hp=%d pos=(%d,%d) act=%s %s goal=%s | dmg=%.0f res=%.0f ai=%d"
                  " | 任务 %d/%d | 回执 %s moved=%d err=%s",
                  p.tick, p.hp, p.self_x, p.self_y, d.action.c_str(),
                  d.dir.c_str(), d.goal.c_str(), p.effective_damage, p.resource_value,
                  llm_->calls(), p.mission_completed,
                  static_cast<int>(p.missions.size()),
                  p.last_action.empty() ? "-" : p.last_action.c_str(),
                  p.last_moved ? 1 : 0,
                  p.last_error.empty() ? "-" : p.last_error.c_str());
    }
    // 被拒的动作单独提示，复盘和现场排错都靠这一行
    if (!p.last_error.empty()) {
      RCLCPP_WARN(get_logger(), "动作被拒：action=%s error=%s（当前 tick=%d）",
                  p.last_action.c_str(), p.last_error.c_str(), p.tick);
    }
  }

  // ---------------------------------------------------------------- 学习
  // 回执里 moved=false 且 error=E_BLOCKED/E_OOB，说明"上一帧从 (x,y) 朝 dir 走"的那格走不通。
  // 记成临时禁行，避免一直撞同一面墙（实测会白白浪费 10~20 个 Tick）。
  void learn_from_receipt(const rmtc::Perception& p) {
    if (!last_cmd_ok_) return;
    if (p.last_moved) return;
    if (p.last_action != "move") return;
    if (p.last_error != "E_BLOCKED" && p.last_error != "E_OOB") return;
    int nx = last_cmd_x_, ny = last_cmd_y_;
    if (last_cmd_dir_ == "up") --ny;
    else if (last_cmd_dir_ == "down") ++ny;
    else if (last_cmd_dir_ == "left") --nx;
    else if (last_cmd_dir_ == "right") ++nx;
    else return;
    world_.mark_temp_block({nx, ny}, p.tick + 15);
    RCLCPP_INFO(get_logger(), "学到障碍：(%d,%d) 走不通，回避 15 Tick", nx, ny);
  }

  // ---------------------------------------------------------------- LLM
  void maybe_ask_llm(const rmtc::Perception& p) {
    if (!llm_->enabled() || llm_->busy()) return;
    if (session_calls_ >= kMaxCallsPerSession) return;  // 本场配额上限，绝不烧光
    const std::string mission = p.mission_text.empty() ? judge_mission_text_ : p.mission_text;
    const std::string event = p.event_text.empty() ? judge_event_text_ : p.event_text;
    if (mission == asked_mission_ && event == asked_event_) return;
    // 公告会随比赛不断累积（event_text 越变越长），必须限频，
    // 否则一场就能打出上百次调用，正式比赛配额（示例 100）瞬间耗尽。
    if (p.tick - last_ask_tick_ < kAskCooldownTicks) return;
    if (p.tick < 5 && !asked_mission_.empty()) return;

    std::string prompt;
    prompt += "你是机器人决策助手。根据赛场公告判断危险区域、方向反转和任务目标。\n";
    prompt += "当前 tick: " + std::to_string(p.tick) + "\n";
    prompt += "地图尺寸: " + std::to_string(p.map_w) + "x" + std::to_string(p.map_h) + "\n";
    prompt += "己方坐标: [" + std::to_string(p.self_x) + "," + std::to_string(p.self_y) + "]\n";
    prompt += "任务公告: " + (mission.empty() ? "(无)" : mission) + "\n";
    prompt += "事件公告: " + (event.empty() ? "(无)" : event) + "\n";
    prompt += kPromptTail;

    if (llm_->ask_async(prompt, p.tick)) {
      asked_mission_ = mission;
      asked_event_ = event;
      last_ask_tick_ = p.tick;
      ++session_calls_;
      RCLCPP_INFO(get_logger(), "已发起 AI 调用（全局第 %d 次，本场第 %d/%d 次）",
                  llm_->calls() + 1, session_calls_, kMaxCallsPerSession);
    }
  }

  void poll_llm(int tick) {
    const std::string err = llm_->take_error();
    if (!err.empty()) {
      RCLCPP_WARN(get_logger(), "AI 调用失败：%.200s", err.c_str());
    }
    auto r = llm_->take_result();
    if (!r) return;
    // 结果必须来自"当前或更早"的 tick；来自未来的 tick 说明是上一场遗留的结果，必须丢弃
    if (r->first < 0 || r->first > tick || tick - r->first > 100) {
      RCLCPP_WARN(get_logger(), "AI 结果 tick=%d 与当前 tick=%d 不匹配，丢弃", r->first, tick);
      return;
    }
    rmtc::Insight ins = rmtc::parse_insight(r->second, r->first);
    if (!ins.valid) {
      RCLCPP_WARN(get_logger(), "AI 输出不是合法 JSON，本次忽略：%.120s", r->second.c_str());
      return;
    }
    insight_ = ins;
    RCLCPP_INFO(get_logger(), "AI 结论：danger=%zu reversed_until=%d target=(%d,%d) %.60s",
                ins.danger.size(), ins.reversed_until_tick, ins.target_x, ins.target_y,
                ins.advice.c_str());
  }

  // ---------------------------------------------------------------- 成员
  static constexpr int kMaxCallsPerSession = 25;  // 本场 AI 调用上限（正式配额示例为 100）
  static constexpr int kAskCooldownTicks = 40;    // 两次调用最小间隔（10s @500ms/Tick）

  std::string seat_, team_name_, mode_, ai_token_, session_;
  bool debug_ = false;
  int session_calls_ = 0;
  // 上一条指令的出发格与方向（用于从 moved=false 回执反推障碍）
  int last_cmd_x_ = -1, last_cmd_y_ = -1;
  std::string last_cmd_dir_;
  bool last_cmd_ok_ = false;
  std::string judge_event_text_, judge_mission_text_;
  std::string asked_mission_, asked_event_;
  int last_ask_tick_ = -1000;
  int last_tick_ = -1;

  rmtc::World world_;
  rmtc::Insight insight_;
  std::unique_ptr<rmtc::LlmClient> llm_;

  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_perception_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_event_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_mission_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_action_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_registration_;
  rclcpp::TimerBase::SharedPtr reg_timer_;
};

}  // namespace

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<Bot>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  // 等可能还在途的 AI 请求结束，避免退出时后台线程访问已销毁的对象
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  return 0;
}
