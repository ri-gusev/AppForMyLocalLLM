#pragma once
#include <string>
#include <vector>

class OllamaClient {
public:
    explicit OllamaClient(std::string model = "qwen3:8b");
    std::string chat(const std::string& prompt);
private:
    struct Message {
        std::string role;
        std::string content;
    };
    std::string model_;
    std::vector<Message> history_;
};
