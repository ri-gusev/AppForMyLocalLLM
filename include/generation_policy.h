#pragma once
#include <string>
#include <vector>

namespace generation {
constexpr int contextWindow = 8192;
constexpr int maxOutputTokens = 512;
constexpr int templateReserve = 256;
constexpr size_t maxHistoryMessages = 16;
struct Message { std::string role, content; };
struct PreparedMessages { std::vector<Message> messages; size_t droppedHistory = 0; };
const std::string& ragPrompt();
size_t inputUpperBound(const std::vector<Message>& messages);
size_t contextByteBudget(const std::string& question);
PreparedMessages prepare(const std::string& question, const std::string& context,
                         const std::vector<Message>& history);
bool isTruncated(const std::string& doneReason, int generatedTokens);
}
