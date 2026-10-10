#include "rag_chat.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    if (argc != 3) return 1;
    auto root = std::filesystem::temp_directory_path() / ("localagent-import-live-" + std::to_string(GetCurrentProcessId()));
    try {
        auto app = std::filesystem::u8path(argv[1]);
        auto fixtures = std::filesystem::u8path(argv[2]);
        auto docx = extractDocument(fixtures / "sample.docx", app);
        require(docx[0].text.find("САПФИР") != std::string::npos, "C++ DOCX subprocess extraction failed");
        auto pdf = extractDocument(fixtures / "sample.pdf", app);
        require(pdf[0].text.find("43219") != std::string::npos && pdf[0].page == 1, "C++ PDF subprocess extraction failed");
        std::filesystem::create_directories(root / "config");
        std::ofstream(root / "config/rag_config.json") << R"({"pre_filter_top_k":10,"post_filter_top_k":4,"similarity_threshold":0.45})";
        auto file = root / std::filesystem::u8path("аврора.md");
        std::ofstream(file, std::ios::binary) << "# Проект Аврора\nРезервные копии проекта Аврора хранятся 37 дней. После истечения этого срока архив автоматически удаляется.\n";
        RagChat chat(root);
        require(chat.addFile(file) == 1, "Live indexing failed");
        RagChat reopened(root);
        auto hits = reopened.retrieve("На какой срок сохраняются бэкапы проекта Аврора?");
        require(!hits.empty() && hits[0].file == "аврора.md", "Persisted file not found by semantic search");
        auto reply = reopened.ask("На какой срок сохраняются бэкапы проекта Аврора?");
        require(reply.answer.find("37") != std::string::npos, "Generated answer missing imported fact");
        require(!reply.sources.empty() && reply.sources[0].file == "аврора.md", "Answer sources missing uploaded file");
        std::filesystem::remove_all(root);
        std::cout << "PASS: C++ PDF/DOCX extraction, live bge-m3 indexing, persisted semantic search, local Qwen answer\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
