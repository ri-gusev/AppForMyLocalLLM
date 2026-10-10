#pragma once
#include "ollama_client.h"
#include "document_import.h"
#include <filesystem>
#include <string>
#include <vector>

struct RagHit {
    std::string file, title, section, chunkId, content;
    int page = 0, lineStart = 0, lineEnd = 0;
    double score = 0, relevance = 0;
};
struct RagReply {
    std::string answer;
    std::vector<RagHit> sources;
    long long retrievalMs = 0, generationMs = 0;
    std::string displayText() const;
};
class RagChat {
public:
    explicit RagChat(std::filesystem::path baseDirectory);
    RagReply ask(const std::string& question);
    std::vector<RagHit> retrieve(const std::string& question) const;
    size_t addFile(const std::filesystem::path& file, const IndexProgress& progress = {});
private:
    std::filesystem::path base_;
    OllamaClient model_;
};
