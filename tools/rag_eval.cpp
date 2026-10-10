#include "rag_chat.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: rag_eval <directory containing data and config> <output.json> [temperature]\n";
        return 1;
    }
    double temperature = 0;
    if (argc == 4) {
        try { temperature = std::stod(argv[3]); }
        catch (...) { std::cerr << "Invalid temperature\n"; return 1; }
    }
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"Какая модель эмбеддингов используется по умолчанию в DocumentIndexer::embedTexts?", "bge-m3"},
        {"Какова максимальная длина query в DocumentIndexer::validSearchRequest?", "16000"},
        {"Какой алгоритм сходства используется в DocumentIndexer::rankVectors?", "cosine"}
    };
    nlohmann::json report = {{"generation_model", "qwen3:8b"}, {"embedding_model", "bge-m3"},
        {"cloud_comparison", "not run: no cloud provider configured in this application"},
        {"quality_check", "case-insensitive expected-term check; not a complete factuality evaluation"},
        {"profile", {{"num_ctx", generation::contextWindow}, {"num_predict", generation::maxOutputTokens}, {"temperature", temperature}}},
        {"runs", nlohmann::json::array()}};
    int errors = 0, passed = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        for (int repeat = 1; repeat <= 2; ++repeat) {
            nlohmann::json run = {{"case", i + 1}, {"repeat", repeat}, {"question", cases[i].first}};
            try {
                // Independent conversations make repetitions comparable.
                RagChat chat(std::filesystem::u8path(argv[1]), temperature);
                auto reply = chat.ask(cases[i].first);
                run["answer"] = reply.answer;
                run["retrieval_ms"] = reply.retrievalMs;
                run["generation_ms"] = reply.generationMs;
                run["input_tokens"] = reply.generation.inputTokens;
                run["output_tokens"] = reply.generation.outputTokens;
                run["input_upper_bound"] = reply.generation.inputUpperBound;
                run["truncated"] = reply.generation.truncated;
                run["context_reduced"] = reply.contextReduced;
                run["invalid_citations"] = reply.invalidCitations;
                run["tokens_per_second"] = reply.generation.evalDurationNs > 0 ?
                    reply.generation.outputTokens * 1e9 / reply.generation.evalDurationNs : 0;
                run["sources"] = nlohmann::json::array();
                for (const auto& hit : reply.sources)
                    run["sources"].push_back({{"file", hit.file}, {"chunk_id", hit.chunkId},
                        {"cosine", hit.score}, {"relevance", hit.relevance}, {"content", hit.content}});
                std::string normalized = reply.answer;
                for (char& ch : normalized) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                const bool found = normalized.find(cases[i].second) != std::string::npos ||
                    (i == 2 && (reply.answer.find("косинус") != std::string::npos || reply.answer.find("Косинус") != std::string::npos));
                const bool ok = found && !reply.sources.empty();
                run["expected_term"] = cases[i].second;
                run["expected_term_present"] = ok;
                if (ok) ++passed;
            } catch (const std::exception& error) {
                run["error"] = error.what();
                ++errors;
            }
            report["runs"].push_back(std::move(run));
            std::cerr << "Case " << i + 1 << ", repeat " << repeat << " done\n";
        }
    }
    report["successful_requests"] = 6 - errors;
    report["expected_term_passes"] = passed;
    std::ofstream output(std::filesystem::u8path(argv[2]), std::ios::binary);
    if (!output) { std::cerr << "Cannot write evaluation report\n"; return 1; }
    output << report.dump(2) << '\n';
    std::cout << "Requests: " << 6 - errors << "/6; expected-term checks: " << passed << "/6\n";
    return errors ? 1 : 0;
}
