#pragma once
#include <string>
#include <vector>

class OllamaClient {
public:
    explicit OllamaClient(std::string model = "qwen3:8b");
    std::string chat(const std::string& prompt, const std::string& context = {});
    static std::vector<float> embed(const std::string& text);
private:
    struct Message {
        std::string role;
        std::string content;
    };
    std::string model_;
    std::vector<Message> history_;
};
