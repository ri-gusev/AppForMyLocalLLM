#pragma once
#include <httplib.h>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace local_api {
constexpr size_t maxBodyBytes = 8192;
constexpr size_t maxPromptBytes = 4000;
using Clock = std::chrono::steady_clock;
using Generate = std::function<std::string(const std::string&)>;
using Now = std::function<Clock::time_point()>;
void jsonError(httplib::Response& response, int status, const std::string& code, const std::string& message);

class ChatApi {
public:
    ChatApi(std::string token, Generate generate, Now now = [] { return Clock::now(); });
    // Checks headers and routes before httplib reads the request body.
    bool preflight(const httplib::Request& request, httplib::Response& response);
    void chat(const httplib::Request& request, httplib::Response& response);
private:
    std::string token_;
    Generate generate_;
    Now now_;
    std::mutex mutex_;
    std::deque<Clock::time_point> posts_;
    bool generating_ = false;
};
void configureServer(httplib::Server& server, ChatApi& api);
}
