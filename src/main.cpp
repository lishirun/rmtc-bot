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

    const std::string ai_url =
        env_or("RMTC_AI_URL", "http://127.0.0.1:8000/v1/chat/completions");
    ai_token_ = env_or("RMTC_TEAM_TOKEN", "");
    const std::string model = env_or("RMTC_AI_MODEL", "");
    llm_ = std::make_unique<rmtc::LlmClient>(ai_url, ai_token_, model);
    llm_->set_max_calls(std::atoi(env_or("RMTC_AI_MAX_CALLS", "40").c_str()));

    auto qos_data = rclcpp::QoS(rclcpp::KeepLast(1)).reliable();
    auto qos_keep = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();

    sub_perception_ = create_subscription<std_msgs::msg::String>(
        "/team_" + seat_ + "/perception", qos_data,
        [this](std_msgs::msg::String::SharedPtr msg) { on_perception(msg->data); });

    pub_action_ = create_publisher<std_msgs::msg::String>(
        "/team_" + seat_ + "/action", qos_data);
    pub_registration_ = create_publisher<std_msgs::msg::String>(
        "/team_" + seat_ + "/registration", qos_keep);

    // 事件与任务是纯文本公告；事件话题 depth 128，允许重复内容，不要当 JSON 解析
    sub_event_ = create_subscription<std_msgs::msg::String>(
        "/referee/event_text", qos_keep,
        [this](std_msgs::msg::String::SharedPtr msg) { judge_event_text_ = msg->data; });
    sub_mission_ = create_subscription<std_msgs::msg::String>(
        "/referee/mission", qos_keep,
        [this](std_msgs::msg::String::SharedPtr msg) { judge_mission_text_ = msg->data; });

    reg_timer_ = create_wall_timer(std::chrono::seconds(5),
                                   [this]() { publish_registration(); });
    publish_registration();

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
  void on_perception(const std::string& raw) {
    auto parsed = rmtc::parse_perception(raw);
    if (!parsed) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "感知解析失败，本 Tick 不发动作");
      return;
    }
    const rmtc::Perception& p = *parsed;

    if (p.tick < last_tick_) return;  // 迟到的感知直接丢弃
    last_tick_ = p.tick;

    if (p.session_id != session_) {  // 换场：清空上一场的缓存
      session_ = p.session_id;
      world_ = rmtc::World();
      insight_ = rmtc::Insight();
      asked_mission_.clear();
      asked_event_.clear();
      last_ask_tick_ = -1000;
      RCLCPP_INFO(get_logger(), "新场次 session_id=%s", session_.c_str());
    }

    world_.init(p.map_w, p.map_h);
    world_.observe(p);

    maybe_ask_llm(p);
    poll_llm(p.tick);

    const rmtc::Decision d = rmtc::decide(p, world_, insight_, p.tick, mode_);

    std_msgs::msg::String out;
    out.data = rmtc::action_to_json(p, d);
    pub_action_->publish(out);

    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000,
                         "tick=%d hp=%d pos=(%d,%d) act=%s %s | dmg=%.0f res=%.0f ai=%d",
                         p.tick, p.hp, p.self_x, p.self_y, d.action.c_str(),
                         d.dir.c_str(), p.effective_damage, p.resource_value,
                         llm_->calls());
  }

  // ---------------------------------------------------------------- LLM
  void maybe_ask_llm(const rmtc::Perception& p) {
    if (!llm_->enabled() || llm_->busy()) return;
    const std::string mission = p.mission_text.empty() ? judge_mission_text_ : p.mission_text;
    const std::string event = p.event_text.empty() ? judge_event_text_ : p.event_text;
    if (mission == asked_mission_ && event == asked_event_) return;
    if (p.tick - last_ask_tick_ < 10) return;  // 冷却，避免把配额烧光

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
      RCLCPP_INFO(get_logger(), "已发起 AI 调用（第 %d 次）", llm_->calls() + 1);
    }
  }

  void poll_llm(int tick) {
    auto r = llm_->take_result();
    if (!r) return;
    if (tick - r->first > 100) return;  // 结果太旧，丢掉
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
  std::string seat_, team_name_, mode_, ai_token_, session_;
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
