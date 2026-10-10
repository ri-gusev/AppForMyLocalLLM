#include "document_import.h"
#include <windows.h>
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int count(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(db, "SELECT count(*) FROM document_chunks", -1, &statement, nullptr) == SQLITE_OK,
            "Count query failed");
    sqlite3_step(statement);
    int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return result;
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("localagent-import-test-" + std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(root);
        auto file = root / std::filesystem::u8path("документ.txt");
        std::ofstream(file, std::ios::binary) << "\xef\xbb\xbf" << "Первый документ.\nКод проекта — САПФИР.";
        auto parts = extractDocument(file, root);
        require(parts.size() == 1 && parts[0].text.find("Первый") == 0, "UTF-8 BOM extraction failed");
        auto chunks = chunkDocument(parts);
        require(chunks.size() == 1 && chunks[0].lineStart == 1 && chunks[0].lineEnd == 2, "Source lines incorrect");
        auto longChunks = chunkDocument({{std::string(6000, 'a'), 3}});
        require(longChunks.size() > 1 && longChunks[0].page == 3, "Long text not split");
        for (const auto& chunk : longChunks) require(chunk.text.size() <= 2200, "Oversized chunk");
        require(chunkDocument({{" \n\t", 0}}).empty(), "Whitespace was indexed");
        auto embed = [](const std::string&) { return std::vector<float>(1024, 0.25f); };
        auto database = root / "user_index.db";
        require(indexDocument(database, file, root, embed) == 1, "Indexing failed");
        sqlite3* db = nullptr;
        require(sqlite3_open(database.string().c_str(), &db) == SQLITE_OK, "Open failed");
        require(count(db) == 1, "Missing indexed chunk");
        require(indexDocument(database, file, root, embed) == 1 && count(db) == 1, "Duplicate import duplicated chunks");
        std::ofstream(file) << std::string(6000, 'a');
        int calls = 0;
        bool failed = false;
        try {
            indexDocument(database, file, root, [&](const std::string&) {
                if (++calls == 2) throw std::runtime_error("simulated unavailable Ollama");
                return std::vector<float>(1024, 0.25f);
            });
        } catch (const std::exception&) { failed = true; }
        require(failed && count(db) == 1, "Failed reindex damaged old document");
        require(sqlite3_exec(db, "CREATE TRIGGER reject_import BEFORE INSERT ON document_chunks BEGIN SELECT RAISE(ABORT,'simulated write failure'); END;", nullptr, nullptr, nullptr) == SQLITE_OK,
                "Trigger setup failed");
        failed = false;
        try { indexDocument(database, file, root, embed); }
        catch (const std::exception&) { failed = true; }
        require(failed && count(db) == 1, "Write failure did not roll back document replacement");
        sqlite3_exec(db, "DROP TRIGGER reject_import", nullptr, nullptr, nullptr);
        {
            std::ofstream utf16(root / "utf16.txt", std::ios::binary);
            const unsigned char bytes[] = {0xff, 0xfe, 0x1f, 0x04, 0x40, 0x04, 0x38, 0x04, 0x32, 0x04, 0x35, 0x04, 0x42, 0x04};
            utf16.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        }
        require(extractDocument(root / "utf16.txt", root)[0].text == "Привет", "UTF-16 extraction failed");
        std::ofstream(root / "cp1251.txt", std::ios::binary) << "\xcf\xf0\xe8\xe2\xe5\xf2";
        require(extractDocument(root / "cp1251.txt", root)[0].text == "Привет", "CP1251 extraction failed");
        std::ofstream(root / "empty.md") << " \n ";
        failed = false;
        try { indexDocument(database, root / "empty.md", root, embed); }
        catch (const std::exception&) { failed = true; }
        require(failed && count(db) == 1, "Empty document changed index");
        failed = false;
        try { indexDocument(database, file, root, [](const std::string&) { return std::vector<float>{1, 2}; }); }
        catch (const std::exception&) { failed = true; }
        require(failed && count(db) == 1, "Wrong embedding dimension changed index");
        sqlite3_close(db);
        std::filesystem::remove_all(root);
        std::cout << "PASS: extraction, chunking, indexing, deduplication, atomic failure recovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
