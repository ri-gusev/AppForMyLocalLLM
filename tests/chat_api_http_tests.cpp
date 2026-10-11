#include "chat_api.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        int calls = 0;
        local_api::ChatApi api("http-test-key", [&](const std::string& text) { ++calls; return "reply: " + text; });
        httplib::Server server;
        local_api::configureServer(server, api);
        int port = server.bind_to_any_port("127.0.0.1");
        require(port > 0, "Loopback bind failed");
        std::thread listener([&] { server.listen_after_bind(); });
        struct Stop { httplib::Server& server; std::thread& thread; ~Stop() { server.stop(); thread.join(); } } stop{server, listener};
        server.wait_until_ready();
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(5, 0);
        auto page = client.Get("/");
        require(page && page->status == 200, "Chat page is not publicly available");
        require(page->get_header_value("Content-Type").find("text/html") == 0,
                "Chat page must be HTML");
        require(page->body.find("name=\"viewport\"") != std::string::npos,
                "Chat page must support mobile viewports");
        require(page->body.find("http-test-key") == std::string::npos,
                "Server token leaked into chat page");
        require(page->get_header_value("Cache-Control") == "no-store",
                "Chat page must not be cached");
        auto wrongMethod = client.Get("/chat");
        require(wrongMethod && wrongMethod->status == 405, "Chat method restriction changed");
        auto health = client.Get("/health");
        require(health && health->status == 200 && nlohmann::json::parse(health->body)["status"] == "ok", "HTTP health failed");
        auto unknown = client.Get("/api/tags");
        require(unknown && unknown->status == 404 && nlohmann::json::parse(unknown->body).contains("error"), "Unknown route exposed");
        auto unauthorized = client.Post("/chat", R"({"prompt":"hello"})", "application/json");
        require(unauthorized && unauthorized->status == 401, "HTTP auth failed");
        httplib::Headers auth{{"Authorization", "Bearer http-test-key"}};
        auto valid = client.Post("/chat", auth, R"({"prompt":"hello"})", "application/json");
        require(valid && valid->status == 200 && nlohmann::json::parse(valid->body)["response"] == "reply: hello", "HTTP success failed");
        auto oversized = client.Post("/chat", auth, std::string(8193, 'x'), "application/json");
        if (!oversized || oversized->status != 413) std::cerr << "Oversize status=" << (oversized ? oversized->status : -1) << " transport=" << static_cast<int>(oversized.error()) << '\n';
        require(oversized && oversized->status == 413, "HTTP body limit failed");
        auto badType = client.Post("/chat", auth, "hello", "text/plain");
        require(badType && badType->status == 415, "HTTP content type failed");
        // Exercise the streaming body bound, not just Content-Length validation.
        auto chunked = client.Post("/chat", auth, [](size_t offset, httplib::DataSink& sink) {
            if (offset >= 9000) { sink.done(); return true; }
            std::string bytes(1000, 'x'); return sink.write(bytes.data(), bytes.size());
        }, "application/json");
        require(chunked && chunked->status == 413 && nlohmann::json::parse(chunked->body).contains("error"), "Chunked limit failed");
        auto limited = client.Post("/chat", auth, R"({"prompt":"hello"})", "application/json");
        require(limited && limited->status == 429 && limited->has_header("Retry-After"), "HTTP global rate limit failed");
        require(calls == 1, "Rejected HTTP requests reached generation");
        std::cout << "PASS: real loopback HTTP, health, auth, JSON, routing, fixed/chunked bounds, rate limit; fake generator only\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
