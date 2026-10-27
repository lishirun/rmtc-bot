#include "llm.hpp"

#include <cstring>
#include <iostream>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace rmtc {

namespace {
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  static_cast<std::string*>(userdata)->append(ptr, total);
  return total;
}
}  // namespace

LlmClient::LlmClient(std::string url, std::string token, std::string model)
    : url_(std::move(url)), token_(std::move(token)), model_(std::move(model)),
      busy_(false), calls_(0) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

bool LlmClient::post(const std::string& body, std::string& out, long timeout_ms) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Expect:");
  const std::string auth = "Authorization: Bearer " + token_;
  headers = curl_slist_append(headers, auth.c_str());

  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) return false;
  if (code < 200 || code >= 300) {
    // 401 令牌错 / 429 配额或限流 / 5xx 网关问题 —— 一律降级，不重试
    std::cerr << "[llm] http " << code << " body=" << out.substr(0, 200) << std::endl;
    return false;
  }
  return true;
}

bool LlmClient::ask_async(const std::string& prompt, int tick) {
  if (!enabled()) return false;
  if (calls_.load() >= max_calls_) return false;
  if (busy_.exchange(true)) return false;  // 同一时刻只允许一个在途请求

  nlohmann::json req;
  req["stream"] = false;
  nlohmann::json user_msg;
  user_msg["role"] = "user";
  user_msg["content"] = prompt;
  nlohmann::json msgs = nlohmann::json::array();
  msgs.push_back(user_msg);
  req["messages"] = msgs;
  if (!model_.empty()) req["model"] = model_;  // 留空则用主办方默认模型
  const std::string body = req.dump();

  std::thread([this, body, tick]() {
    std::string raw;
    const bool ok = post(body, raw, timeout_ms_);
    if (ok) {
      nlohmann::json resp = nlohmann::json::parse(raw, nullptr, false);
      if (!resp.is_discarded()) {
        try {
          const std::string content =
              resp["choices"][0]["message"]["content"].get<std::string>();
          {
            std::lock_guard<std::mutex> lock(mu_);
            result_ = std::make_pair(tick, content);
          }
          calls_.fetch_add(1);  // 只有成功返回才计配额
        } catch (...) {
          std::cerr << "[llm] 响应结构异常" << std::endl;
        }
      }
    }
    busy_.store(false);
  }).detach();
  return true;
}

std::optional<std::pair<int, std::string> > LlmClient::take_result() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!result_) return std::nullopt;
  auto r = result_;
  result_.reset();
  return r;
}

}  // namespace rmtc
