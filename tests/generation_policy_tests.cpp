#include "generation_policy.h"
#include "rag_context.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        std::vector<generation::Message> history;
        for (int i = 0; i < 20; ++i) {
            history.push_back({"user", std::string(500, 'u')});
            history.push_back({"assistant", std::string(500, 'a')});
        }
        auto prepared = generation::prepare("current question", "documents", history);
        require(prepared.droppedHistory > 0, "History was not bounded");
        require(generation::inputUpperBound(prepared.messages) + generation::maxOutputTokens <= generation::contextWindow,
                "Context and completion exceed window");
        require(prepared.messages.front().role == "system" && prepared.messages.back().content == "current question",
                "Question or grounding lost");
        require((prepared.messages.size() - 2) % 2 == 0, "Incomplete history pairs retained");
        bool rejected = false;
        try { generation::prepare(std::string(9000, 'x'), "", {}); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "Oversized question silently truncated");
        require(generation::isTruncated("length", 512) && !generation::isTruncated("stop", 512), "Completion reason mishandled");
        RagHit a; a.file = "a.md"; a.chunkId = "1"; a.content = "A specific supported fact.\nAnother sentence.";
        RagHit duplicate = a; duplicate.chunkId = "2"; duplicate.content = "A specific supported fact.  Another sentence.";
        RagHit distinct = a; distinct.chunkId = "3"; distinct.content = "A different supported fact.";
        require(deduplicateSources({a, duplicate, distinct}).size() == 2, "Duplicate removal lost unique evidence");
        std::vector<RagHit> hits;
        for (int i = 0; i < 4; ++i) {
            RagHit hit; hit.file = "file" + std::to_string(i); hit.chunkId = std::to_string(i);
            for (int j = 0; j < 1800; ++j) hit.content += "Ж";
            hits.push_back(hit);
        }
        auto context = prepareRagContext("question", hits);
        require(context.reduced && !context.sources.empty(), "Document context was not reduced");
        require(context.json.size() <= generation::contextByteBudget("question"), "Document context exceeds budget");
        auto parsed = nlohmann::json::parse(context.json);
        require(parsed.size() == context.sources.size(), "Displayed sources differ from actual context");
        generation::prepare("question", context.json, history);
        RagReply truncated;
        truncated.answer = "Partial answer";
        truncated.generation.truncated = true;
        require(truncated.displayText().find("Ответ обрезан") != std::string::npos, "Truncation warning missing from UI text");
        RagHit changed = a;
        changed.content = a.content + " The limit is now 47, not 37.";
        require(deduplicateSources({a, changed}).size() == 2, "Distinct facts were mistaken for duplicates");
        bool invalid = false;
        require(normalizeCitations("Fact [id:2]. Other [1].", 2, invalid) == "Fact [2]. Other [1]." && !invalid,
                "Citation normalization failed");
        normalizeCitations("Fact [9].", 2, invalid);
        require(invalid, "Unknown citation not flagged");
        require(normalizeCitations("Code `arr[99]` and [id2].", 2, invalid) == "Code `arr[99]` and [2]." && !invalid,
                "Code literal was mistaken for citation");
        std::cout << "PASS: input/output budget, complete history turns, long-question rejection, truncation, deduplication, UTF-8 document context\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
