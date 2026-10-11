#include "chat_api.h"
#include "web_chat.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>

namespace local_api {
namespace {
std::string lower(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
bool sameToken(const std::string& provided, const std::string& expected) {
    size_t difference = provided.size() ^ expected.size();
    for (size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i]) ^
                      (i < provided.size() ? static_cast<unsigned char>(provided[i]) : 0);
    return difference == 0;
}
}
void jsonError(httplib::Response& res, int status, const std::string& code, const std::string& message) {
    res.status = status;
    res.set_content(nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}
ChatApi::ChatApi(std::string token, Generate generate, Now now)
    : token_(std::move(token)), generate_(std::move(generate)), now_(std::move(now)) {
    if (token_.empty() || token_.size() > 4096 ||
        std::any_of(token_.begin(), token_.end(), [](unsigned char ch) { return ch < 33 || ch > 126; }))
        throw std::runtime_error("LOCAL_AGENT_API_KEY must contain a nonempty printable ASCII token without whitespace");
    if (!generate_ || !now_) throw std::runtime_error("Missing API callback");
}
bool ChatApi::preflight(const httplib::Request& req, httplib::Response& res) {
    if (req.method == "POST") {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = now_();
        while (!posts_.empty() && now - posts_.front() >= std::chrono::seconds(60)) posts_.pop_front();
        if (posts_.size() >= 5) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(posts_.front() + std::chrono::seconds(60) - now).count();
            jsonError(res, 429, "rate_limit", "At most 5 POST requests are allowed in 60 seconds");
            res.set_header("Retry-After", std::to_string(std::max<long long>(1, (remaining + 999) / 1000)));
            return false;
        }
        posts_.push_back(now);
    }
    if (req.path != "/" && req.path != "/health" && req.path != "/chat") {
        jsonError(res, 404, "not_found", "Unknown endpoint"); return false;
    }
    const auto method = req.path == "/chat" ? "POST" : "GET";
    if (req.method != method) {
        jsonError(res, 405, "method_not_allowed", "Unsupported method for this endpoint");
        res.set_header("Allow", method); return false;
    }
    if (req.path == "/chat") {
        const auto auth = req.get_header_value("Authorization");
        if (req.get_header_value_count("Authorization") != 1 || auth.size() < 7 ||
            lower(auth.substr(0, 7)) != "bearer " || !sameToken(auth.substr(7), token_)) {
            jsonError(res, 401, "unauthorized", "A valid Bearer token is required");
            res.set_header("WWW-Authenticate", "Bearer"); return false;
        }
        const auto contentType = req.get_header_value("Content-Type");
        if (req.get_header_value_count("Content-Type") != 1 ||
            lower(trim(contentType.substr(0, contentType.find(';')))) != "application/json") {
            jsonError(res, 415, "unsupported_media_type", "Content-Type must be application/json"); return false;
        }
        if (req.has_header("Content-Encoding") && lower(req.get_header_value("Content-Encoding")) != "identity") {
            jsonError(res, 415, "unsupported_encoding", "Compressed request bodies are not supported"); return false;
        }
    }
    if (req.get_header_value_count("Content-Length") > 1 ||
        (req.has_header("Content-Length") && req.has_header("Transfer-Encoding"))) {
        jsonError(res, 400, "invalid_framing", "Ambiguous request body framing"); return false;
    }
    if (req.has_header("Content-Length")) {
        const auto value = req.get_header_value("Content-Length");
        unsigned long long length = 0;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
        if (value.empty() || parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
            jsonError(res, 400, "invalid_content_length", "Content-Length must be a nonnegative integer"); return false;
        }
        if (length > maxBodyBytes) {
            jsonError(res, 413, "body_too_large", "Request body exceeds 8192 bytes"); return false;
        }
    }
    if (req.path == "/chat") {
        if (!req.has_header("Content-Length") && !req.has_header("Transfer-Encoding")) {
            jsonError(res, 411, "length_required", "Use Content-Length or chunked transfer encoding"); return false;
        }
        if (req.has_header("Transfer-Encoding") && (req.get_header_value_count("Transfer-Encoding") != 1 ||
            lower(trim(req.get_header_value("Transfer-Encoding"))) != "chunked")) {
            jsonError(res, 400, "invalid_framing", "Only chunked transfer encoding is supported"); return false;
        }
    }
    if (req.path == "/chat") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generating_) {
            jsonError(res, 429, "generation_busy", "A generation is already running; retry later");
            res.set_header("Retry-After", "1"); return false;
        }
    }
    return true;
}
void ChatApi::chat(const httplib::Request& req, httplib::Response& res) {
    if (req.body.size() > maxBodyBytes) {
        jsonError(res, 413, "body_too_large", "Request body exceeds 8192 bytes"); return;
    }
    const auto json = nlohmann::json::parse(req.body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.size() != 1 ||
        !json.contains("prompt") || !json["prompt"].is_string()) {
        jsonError(res, 400, "invalid_json", "Expected a JSON object containing only a string prompt"); return;
    }
    const auto prompt = json["prompt"].get<std::string>();
    if (prompt.size() > maxPromptBytes) {
        jsonError(res, 413, "prompt_too_large", "Prompt exceeds 4000 UTF-8 bytes"); return;
    }
    if (trim(prompt).empty() || prompt.find('\0') != prompt.npos) {
        jsonError(res, 400, "invalid_prompt", "Prompt must be nonempty text without null characters"); return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generating_) {
            jsonError(res, 429, "generation_busy", "A generation is already running; retry later");
            res.set_header("Retry-After", "1"); return;
        }
        generating_ = true;
    }
    struct Release {
        std::mutex& mutex; bool& active;
        ~Release() { std::lock_guard<std::mutex> lock(mutex); active = false; }
    } release{mutex_, generating_};
    try {
        const auto answer = generate_(prompt);
        res.status = 200;
        res.set_content(nlohmann::json{{"response", answer}}.dump(), "application/json; charset=utf-8");
    } catch (const std::exception& error) {
        const bool timeout = std::string(error.what()).find("12002") != std::string::npos;
        jsonError(res, timeout ? 504 : 502, timeout ? "model_timeout" : "model_unavailable",
                  timeout ? "Local model request timed out" : "Local model request failed; check Ollama and qwen3:8b");
    } catch (...) {
        jsonError(res, 502, "model_unavailable", "Local model request failed");
    }
}
void configureServer(httplib::Server& server, ChatApi& api) {
    server.new_task_queue = [] { return new httplib::ThreadPool(4, 16); };
    server.set_payload_max_length(maxBodyBytes);
    server.set_read_timeout(15, 0); // Receiving a small request, not waiting for model inference.
    server.set_write_timeout(240, 0);
    server.set_keep_alive_max_count(1);
    server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}, {"Connection", "close"}});
    server.set_pre_routing_handler([&api](const httplib::Request& req, httplib::Response& res) {
        if (api.preflight(req, res)) return httplib::Server::HandlerResponse::Unhandled;
        // Bounded draining prevents a Windows TCP reset from hiding the JSON rejection.
        // Oversized/unframed/ambiguous uploads are never buffered or sent to the model.
        if (req.method == "POST" && req.path == "/chat" && req.get_header_value_count("Content-Length") == 1 &&
            !req.has_header("Transfer-Encoding")) {
            const auto value = req.get_header_value("Content-Length");
            unsigned long long length = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() && length <= 65536)
                return httplib::Server::HandlerResponse::Unhandled;
        }
        return httplib::Server::HandlerResponse::Handled;
    });
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content(webChatPage(), "text/html; charset=utf-8");
        res.set_header("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header("X-Frame-Options", "DENY");
    });
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200; res.set_content(R"({"status":"ok"})", "application/json; charset=utf-8");
    });
    server.Post("/chat", [&api](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
        if (res.status >= 400 && !res.body.empty()) {
            const int status = res.status;
            const auto errorBody = res.body;
            reader([](const char*, size_t) { return true; });
            res.status = status;
            res.set_content(errorBody, "application/json; charset=utf-8");
            return;
        }
        std::string body;
        bool tooLarge = false;
        size_t received = 0;
        const bool read = reader([&](const char* data, size_t length) {
            if (length > 65536 - received) { tooLarge = true; return false; }
            received += length;
            if (tooLarge || length > maxBodyBytes - body.size()) { tooLarge = true; return true; }
            body.append(data, length); return true;
        });
        if (tooLarge) { jsonError(res, 413, "body_too_large", "Request body exceeds 8192 bytes"); return; }
        if (!read) { jsonError(res, 400, "invalid_body", "Request body could not be read"); return; }
        auto complete = req;
        complete.body = std::move(body);
        api.chat(complete, res);
    });
    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.get_header_value("Content-Type").find("application/json") == 0 && !res.body.empty()) return;
        jsonError(res, res.status, res.status == 413 ? "body_too_large" : "http_error",
                  res.status == 413 ? "Request body exceeds 8192 bytes" : "HTTP request could not be processed");
    });
    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        jsonError(res, 500, "internal_error", "Internal server error");
    });
}
}
