#include "chat_api.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
httplib::Request request(std::string body = R"({"prompt":"hello"})") {
    httplib::Request req;
    req.method = "POST"; req.path = "/chat"; req.body = std::move(body);
    req.set_header("Authorization", "Bearer test-only-key");
    req.set_header("Content-Type", "application/json; charset=utf-8");
    req.set_header("Content-Length", std::to_string(req.body.size()));
    return req;
}
httplib::Response dispatch(local_api::ChatApi& api, const httplib::Request& req) {
    httplib::Response response;
    if (api.preflight(req, response)) {
        if (req.method == "GET") { response.status = 200; response.set_content(R"({"status":"ok"})", "application/json"); }
        else api.chat(req, response);
    }
    require(nlohmann::json::parse(response.body).is_object(), "Response must be a JSON object");
    return response;
}
int main() {
    try {
        int calls = 0;
        auto generate = [&](const std::string& text) { ++calls; return text; };
        auto check = [&](httplib::Request req, int status) {
            local_api::ChatApi api("test-only-key", generate);
            require(dispatch(api, req).status == status, "Unexpected validation/auth status");
        };
        bool rejected = false;
        try { local_api::ChatApi api("", generate); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Missing startup key accepted");
        auto req = request(); req.headers.erase("Authorization"); check(req, 401);
        req = request(); req.headers.erase("Authorization"); req.set_header("Authorization", "Bearer incorrect"); check(req, 401);
        req = request(); req.set_header("Authorization", "Bearer test-only-key"); check(req, 401);
        req = request(); req.headers.erase("Content-Type"); req.set_header("Content-Type", "text/plain"); check(req, 415);
        check(request("{broken"), 400);
        check(request(R"({"prompt":23})"), 400);
        check(request(R"({"prompt":"   "})"), 400);
        check(request(nlohmann::json{{"prompt", std::string(4001, 'x')}}.dump()), 413);
        check(request(std::string(8193, 'x')), 413);
        req = request(); req.headers.erase("Content-Length"); req.set_header("Content-Length", "8193"); check(req, 413);
        req = request(); req.path = "/api/generate"; check(req, 404);
        req = request(); req.method = "GET"; check(req, 405);
        req = request(); req.path = "/"; check(req, 405);
        require(calls == 0, "Invalid requests reached model");
        check(request(nlohmann::json{{"prompt", std::string(4000, 'x')}}.dump()), 200);
        auto now = local_api::Clock::time_point{};
        local_api::ChatApi limited("test-only-key", generate, [&] { return now; });
        for (int i = 0; i < 5; ++i) require(dispatch(limited, request()).status == 200, "Rate allowance too small");
        auto denied = dispatch(limited, request());
        require(denied.status == 429 && denied.has_header("Retry-After"), "Sixth POST not rate limited");
        httplib::Request health; health.method = "GET"; health.path = "/health";
        require(dispatch(limited, health).status == 200, "Health affected by POST limit");
        httplib::Request page; page.method = "GET"; page.path = "/";
        httplib::Response pageResponse;
        require(limited.preflight(page, pageResponse), "Public chat page affected by auth or POST limit");
        now += std::chrono::seconds(60);
        require(dispatch(limited, request()).status == 200, "Sliding window did not expire");
        local_api::ChatApi unauthorized("test-only-key", generate);
        req = request(); req.headers.erase("Authorization");
        for (int i = 0; i < 5; ++i) require(dispatch(unauthorized, req).status == 401, "Unauthorized request not handled");
        require(dispatch(unauthorized, req).status == 429, "Unauthorized POSTs bypassed rate limit");
        std::mutex mutex; std::condition_variable cv; bool started = false, release = false;
        local_api::ChatApi concurrent("test-only-key", [&](const std::string&) {
            std::unique_lock<std::mutex> lock(mutex); started = true; cv.notify_all();
            cv.wait(lock, [&] { return release; }); return std::string("reply");
        });
        auto first = std::async(std::launch::async, [&] { return dispatch(concurrent, request()); });
        { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return started; }); }
        const int secondStatus = dispatch(concurrent, request()).status;
        const int healthStatus = dispatch(concurrent, health).status;
        { std::lock_guard<std::mutex> lock(mutex); release = true; } cv.notify_all();
        require(first.get().status == 200 && secondStatus == 429 && healthStatus == 200, "Concurrent generation admission failed");
        local_api::ChatApi failing("test-only-key", [](const std::string&) -> std::string { throw std::runtime_error("sensitive upstream text"); });
        auto failure = dispatch(failing, request());
        require(failure.status == 502 && failure.body.find("sensitive") == std::string::npos, "Upstream details leaked");
        require(dispatch(failing, request()).status == 502, "Generation slot not released after exception");
        std::cout << "PASS: startup key, auth, validation, byte limits, rate window, concurrency, sanitized errors; no model calls\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
