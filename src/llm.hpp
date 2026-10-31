#pragma once
// 统一 AI 网关客户端（OpenAI 兼容，非流式）
//
// 规则约束：
//  · 唯一允许的外部 AI 通道，接口 POST <GATEWAY>/v1/chat/completions
//  · Authorization: Bearer <本场席位令牌>
//  · 必须异步：AI 超时不能阻塞动作窗口
//  · 同一席位最多 2 个在途调用，这里保守限制为 1 个
//  · 只有 HTTP 2xx 才算成功调用（计入配额与"真实调用"资格）

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace rmtc {

class LlmClient {
 public:
  LlmClient(std::string url, std::string token, std::string model);

  void set_max_calls(int n) { max_calls_ = n; }
  void set_timeout_ms(long ms) { timeout_ms_ = ms; }

  int calls() const { return calls_.load(); }
  bool busy() const { return busy_.load(); }
  bool enabled() const { return !url_.empty(); }

  // 非阻塞：真正请求跑在后台线程，结果用 take_result() 取
  bool ask_async(const std::string& prompt, int tick);

  // 取回最近一次完成的结果 (tick, content)；没有则 nullopt
  std::optional<std::pair<int, std::string> > take_result();

  // 换场时清掉上一场遗留的结果 —— 否则旧场次的结论会被带进新场次
  void drop_pending() {
    std::lock_guard<std::mutex> lock(mu_);
    result_.reset();
  }

  // 取走最近一次失败原因（401/404/429 等），用于现场排错；取走后清空
  std::string take_error() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string e;
    e.swap(last_error_);
    return e;
  }

 private:
  bool post(const std::string& body, std::string& out, long timeout_ms);

  std::string url_, token_, model_;
  std::atomic<bool> busy_;
  std::atomic<int> calls_;
  int max_calls_ = 40;
  long timeout_ms_ = 1500;

  std::mutex mu_;
  std::optional<std::pair<int, std::string> > result_;
  std::string last_error_;
};

}  // namespace rmtc
