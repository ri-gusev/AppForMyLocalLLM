#include "ollama_client.h"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
struct HandleCloser {
    void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;
void check(bool success, const char* operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + " failed (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
}

OllamaClient::OllamaClient(std::string model) : model_(std::move(model)) {}

std::string OllamaClient::chat(const std::string& prompt) {
    auto messages = nlohmann::json::array();
    for (const auto& message : history_)
        messages.push_back({{"role", message.role}, {"content", message.content}});
    messages.push_back({{"role", "user"}, {"content", prompt}});
    const auto body = nlohmann::json{
        {"model", model_},
        {"messages", messages},
        {"stream", false}, {"think", false}
    }.dump();
    Handle session(WinHttpOpen(L"LocalAgent/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    check(session != nullptr, "WinHttpOpen");
    check(WinHttpSetTimeouts(session.get(), 5000, 5000, 180000, 180000), "SetTimeouts");
    Handle connection(WinHttpConnect(session.get(), L"localhost", 11434, 0));
    check(connection != nullptr, "WinHttpConnect");
    Handle request(WinHttpOpenRequest(connection.get(), L"POST", L"/api/chat",
                                     nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    check(request != nullptr, "WinHttpOpenRequest");
    check(WinHttpSendRequest(request.get(), L"Content-Type: application/json; charset=utf-8\r\n",
                             static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                             static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0),
          "WinHttpSendRequest");
    check(WinHttpReceiveResponse(request.get(), nullptr), "WinHttpReceiveResponse");
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    check(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                              WINHTTP_NO_HEADER_INDEX), "WinHttpQueryHeaders");
    std::string response;
    char buffer[8192];
    DWORD bytesRead = 0;
    do {
        check(WinHttpReadData(request.get(), buffer, sizeof(buffer), &bytesRead), "WinHttpReadData");
        response.append(buffer, bytesRead);
    } while (bytesRead > 0);
    if (status < 200 || status >= 300)
        throw std::runtime_error("Ollama HTTP " + std::to_string(status) + ": " + response);
    const auto json = nlohmann::json::parse(response);
    if (json.contains("error")) throw std::runtime_error(json.at("error").get<std::string>());
    auto answer = json.at("message").at("content").get<std::string>();
    history_.push_back({"user", prompt});
    history_.push_back({"assistant", answer});
    return answer;
}
