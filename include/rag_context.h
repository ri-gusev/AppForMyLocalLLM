#pragma once
#include "rag_chat.h"

struct PreparedRagContext {
    std::vector<RagHit> sources;
    std::string json;
    bool reduced = false;
};
std::vector<RagHit> deduplicateSources(std::vector<RagHit> hits);
PreparedRagContext prepareRagContext(const std::string& question, const std::vector<RagHit>& hits);
std::string normalizeCitations(const std::string& answer, size_t sourceCount, bool& invalid);
