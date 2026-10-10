#include "rag_chat.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <windows.h>

void expectFailure(const std::filesystem::path& base, const std::string& expected) {
    try {
        RagChat chat(base);
        chat.retrieve("test");
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) != std::string::npos) return;
        throw;
    }
    throw std::runtime_error("Expected failure: " + expected);
}
int main() {
    auto base = std::filesystem::temp_directory_path() /
        ("localagent-rag-validation-" + std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(base / "config");
        std::filesystem::create_directories(base / "data");
        expectFailure(base, "config missing");
        const auto writeConfig = [&](const nlohmann::json& json) {
            std::ofstream(base / "config/rag_config.json") << json.dump();
        };
        auto config = nlohmann::json{{"pre_filter_top_k", 10}, {"post_filter_top_k", 4}, {"similarity_threshold", 0.45}};
        auto invalid = config;
        invalid["pre_filter_top_k"] = 2.5;
        writeConfig(invalid);
        expectFailure(base, "Invalid RAG config");
        invalid = config;
        invalid["post_filter_top_k"] = 11;
        writeConfig(invalid);
        expectFailure(base, "Invalid RAG config");
        invalid = config;
        invalid["similarity_threshold"] = 1.1;
        writeConfig(invalid);
        expectFailure(base, "Invalid RAG config");
        writeConfig(config);
        expectFailure(base, "Cannot open local RAG index");
        if (std::filesystem::exists(base / "data/document_index.db"))
            throw std::runtime_error("Retrieval unexpectedly created a missing database");
        sqlite3* db = nullptr;
        if (sqlite3_open((base / "data/document_index.db").string().c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("Fixture database failed");
        sqlite3_close(db);
        expectFailure(base, "no such table");
        std::filesystem::remove_all(base);
        std::cout << "PASS: invalid config, missing index, read-only opening, missing schema; no network used\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(base);
        return 1;
    }
}
