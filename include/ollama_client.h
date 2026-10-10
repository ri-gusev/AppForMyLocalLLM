#pragma once
#include <string>
#include <vector>
#include "generation_policy.h"

struct GenerationStats {
    bool truncated = false;
    size_t droppedHistory = 0, inputUpperBound = 0;
    int inputTokens = 0, outputTokens = 0;
    long long evalDurationNs = 0;
};

class OllamaClient {
public:
    explicit OllamaClient(std::string model = "qwen3:8b", double temperature = 0);
    std::string chat(const std::string& prompt, const std::string& context = {});
    static std::vector<float> embed(const std::string& text);
    const GenerationStats& lastStats() const { return stats_; }
private:
    std::string model_;
    std::vector<generation::Message> history_;
    GenerationStats stats_;
    size_t discardedHistory_ = 0;
    double temperature_ = 0;
};
