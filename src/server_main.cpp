#include "chat_api.h"
#include "ollama_client.h"
#include <cstdlib>
#include <iostream>

int main() {
    const char* key = std::getenv("LOCAL_AGENT_API_KEY");
    if (!key || !*key) {
        std::cerr << "Server not started: set LOCAL_AGENT_API_KEY in the process environment.\n";
        return 1;
    }
    try {
        local_api::ChatApi api(key, [](const std::string& prompt) {
            // A fresh client per request: no shared conversation history.
            OllamaClient client("qwen3:8b");
            return client.chat(prompt);
        });
        httplib::Server server;
        local_api::configureServer(server, api);
        if (!server.bind_to_port("127.0.0.1", 8080)) {
            std::cerr << "Cannot bind 127.0.0.1:8080; check whether the port is in use.\n";
            return 1;
        }
        std::cout << "Local Agent API listening on http://127.0.0.1:8080\n"
                  << "GET / for web chat; GET /health; authenticated POST /chat. Stop with Ctrl+C.\n" << std::flush;
        if (!server.listen_after_bind()) {
            std::cerr << "HTTP server stopped after a socket error.\n"; return 1;
        }
    } catch (const std::exception&) {
        std::cerr << "Server not started: invalid API key or server initialization failure.\n";
        return 1;
    }
    return 0;
}
