#include "document_import.h"
#include <windows.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {
struct DbCloser { void operator()(sqlite3* db) const { sqlite3_close(db); } };
struct StatementCloser { void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); } };
struct HandleCloser { void operator()(void* handle) const { if (handle) CloseHandle(handle); } };
using Handle = std::unique_ptr<void, HandleCloser>;
std::string readFile(const std::filesystem::path& file) {
    if (std::filesystem::file_size(file) > 10 * 1024 * 1024) throw std::runtime_error("Файл больше 10 МБ.");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Не удалось открыть файл.");
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::string utf8(const std::wstring& text) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length && !text.empty()) throw std::runtime_error("Некорректная кодировка текста.");
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::string decodeText(std::string bytes) {
    if (bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) ||
        (static_cast<unsigned char>(bytes[0]) == 0xfe && static_cast<unsigned char>(bytes[1]) == 0xff))) {
        if (bytes.size() % 2) throw std::runtime_error("Некорректный UTF-16.");
        bool little = static_cast<unsigned char>(bytes[0]) == 0xff;
        std::wstring text;
        for (size_t i = 2; i < bytes.size(); i += 2) {
            auto a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
            text += static_cast<wchar_t>(little ? a | (b << 8) : (a << 8) | b);
        }
        bytes = utf8(text);
    } else {
        if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
        if (!bytes.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0)) {
            int length = MultiByteToWideChar(1251, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring text(length, L'\0');
            MultiByteToWideChar(1251, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
            bytes = utf8(text);
        }
    }
    if (bytes.find('\0') != bytes.npos) throw std::runtime_error("Файл содержит двоичные данные.");
    bytes.erase(std::remove(bytes.begin(), bytes.end(), '\r'), bytes.end());
    if (bytes.size() > 8 * 1024 * 1024) throw std::runtime_error("Слишком много текста в документе.");
    return bytes;
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++backslashes; continue; }
        result.append(backslashes * (ch == L'\"' ? 2 : 1), L'\\');
        if (ch == L'\"') result += L'\\';
        result += ch;
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'\"';
}
std::vector<DocumentPart> extractOffice(const std::filesystem::path& file, const std::filesystem::path& base) {
    std::wstring python;
    std::ifstream config(base / "config/python_path.txt", std::ios::binary);
    std::string configuredPath;
    std::getline(config, configuredPath);
    if (!configuredPath.empty() && configuredPath.back() == '\r') configuredPath.pop_back();
    if (!configuredPath.empty()) {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configuredPath.data(), static_cast<int>(configuredPath.size()), nullptr, 0);
        python.resize(length);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configuredPath.data(), static_cast<int>(configuredPath.size()), python.data(), length);
    }
    if (python.empty() || !std::filesystem::exists(python)) {
        std::wstring buffer(32768, L'\0');
        DWORD length = SearchPathW(nullptr, L"python.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (!length || length >= buffer.size()) throw std::runtime_error("Для PDF/DOCX требуется установленный Python 3.");
        buffer.resize(length);
        python = std::move(buffer);
    }
    auto script = base / "scripts/extract_document.py";
    if (!std::filesystem::exists(script)) throw std::runtime_error("Не найден scripts/extract_document.py рядом с приложением.");
    wchar_t tempDirectory[MAX_PATH], tempFile[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDirectory) || !GetTempFileNameW(tempDirectory, L"rag", 0, tempFile))
        throw std::runtime_error("Не удалось создать временный файл извлечения.");
    struct TempFile { std::filesystem::path path; ~TempFile() { std::error_code error; std::filesystem::remove(path, error); } } output{tempFile};
    auto command = quote(python) + L" " + quote(script.wstring()) + L" " + quote(file.wstring()) + L" " + quote(output.path.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Не удалось запустить локальный Python для чтения PDF/DOCX.");
    Handle processHandle(process.hProcess), threadHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        throw std::runtime_error("Превышено время извлечения текста из документа.");
    }
    const auto json = nlohmann::json::parse(readFile(output.path));
    if (json.contains("error")) throw std::runtime_error(json.at("error").get<std::string>());
    std::vector<DocumentPart> result;
    for (const auto& part : json.at("parts"))
        result.push_back({decodeText(part.at("text").get<std::string>()), part.at("page").get<int>()});
    return result;
}
void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
std::unique_ptr<sqlite3_stmt, StatementCloser> prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw);
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    return statement;
}
void bindText(sqlite3_stmt* statement, int index, const std::string& text) {
    if (sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("Не удалось сохранить текст документа.");
}
}

std::vector<DocumentPart> extractDocument(const std::filesystem::path& file, const std::filesystem::path& base) {
    if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Не найден выбранный файл.");
    if (std::filesystem::file_size(file) > 10 * 1024 * 1024) throw std::runtime_error("Файл больше 10 МБ.");
    std::wstring extension = file.extension().wstring();
    for (auto& ch : extension) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    if (extension == L".txt" || extension == L".md") return {{decodeText(readFile(file)), 0}};
    if (extension == L".pdf" || extension == L".docx") return extractOffice(file, base);
    throw std::runtime_error("Поддерживаются TXT, Markdown (.md), PDF и DOCX.");
}

std::vector<DocumentChunk> chunkDocument(const std::vector<DocumentPart>& parts) {
    std::vector<DocumentChunk> chunks;
    for (const auto& part : parts) {
        size_t start = 0, counted = 0;
        int line = 1;
        while (start < part.text.size()) {
            size_t end = std::min(start + 2200, part.text.size());
            while (end < part.text.size() && (static_cast<unsigned char>(part.text[end]) & 0xc0) == 0x80) --end;
            if (end < part.text.size()) {
                size_t newline = part.text.rfind('\n', end - 1);
                if (newline != part.text.npos && newline > start + 1100) end = newline + 1;
            }
            for (; counted < start; ++counted) if (part.text[counted] == '\n') ++line;
            auto text = part.text.substr(start, end - start);
            int lastLine = line + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
            if (!text.empty() && text.back() == '\n') --lastLine;
            if (text.find_first_not_of(" \t\r\n") != text.npos) chunks.push_back({std::move(text), part.page, line, lastLine});
            if (end == part.text.size()) break;
            start = end - 250;
            while (start < end && (static_cast<unsigned char>(part.text[start]) & 0xc0) == 0x80) ++start;
        }
    }
    return chunks;
}

size_t indexDocument(const std::filesystem::path& database, const std::filesystem::path& file,
                     const std::filesystem::path& base, const EmbeddingFunction& embed, const IndexProgress& progress) {
    auto chunks = chunkDocument(extractDocument(file, base));
    if (chunks.empty()) throw std::runtime_error("В документе нет текста для индексации.");
    std::vector<std::vector<float>> vectors;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (progress) progress(i, chunks.size());
        auto vector = embed(chunks[i].text);
        if (vector.size() != 1024) throw std::runtime_error("Ожидался 1024-мерный эмбеддинг bge-m3.");
        double norm = 0;
        for (float value : vector) {
            if (!std::isfinite(value)) throw std::runtime_error("Некорректный эмбеддинг документа.");
            norm += static_cast<double>(value) * value;
        }
        if (norm == 0) throw std::runtime_error("Пустой эмбеддинг документа.");
        vectors.push_back(std::move(vector));
    }
    auto sourcePath = std::filesystem::weakly_canonical(file).wstring();
    CharLowerBuffW(sourcePath.data(), static_cast<DWORD>(sourcePath.size()));
    auto source = utf8(sourcePath);
    std::filesystem::create_directories(database.parent_path());
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(database.u8string().c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    std::unique_ptr<sqlite3, DbCloser> db(raw);
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_busy_timeout(db.get(), 5000);
    exec(db.get(), "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS document_chunks("
        "id INTEGER PRIMARY KEY,source TEXT NOT NULL,file TEXT NOT NULL,title TEXT NOT NULL,section TEXT NOT NULL,"
        "chunk_id TEXT UNIQUE NOT NULL,strategy TEXT NOT NULL,content TEXT NOT NULL,embedding BLOB NOT NULL,"
        "embedding_dim INTEGER NOT NULL,page INTEGER NOT NULL,line_start INTEGER NOT NULL,line_end INTEGER NOT NULL);");
    exec(db.get(), "BEGIN IMMEDIATE;");
    try {
        auto remove = prepare(db.get(), "DELETE FROM document_chunks WHERE source=?");
        bindText(remove.get(), 1, source);
        if (sqlite3_step(remove.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        auto insert = prepare(db.get(), "INSERT INTO document_chunks(source,file,title,section,chunk_id,strategy,content,embedding,embedding_dim,page,line_start,line_end) "
            "VALUES(?,?,?,'',?,'semantic',?,?,1024,?,?,?)");
        for (size_t i = 0; i < chunks.size(); ++i) {
            sqlite3_reset(insert.get());
            sqlite3_clear_bindings(insert.get());
            bindText(insert.get(), 1, source);
            bindText(insert.get(), 2, file.filename().u8string());
            bindText(insert.get(), 3, file.filename().u8string());
            bindText(insert.get(), 4, source + ":semantic:" + std::to_string(i + 1));
            bindText(insert.get(), 5, chunks[i].text);
            if (sqlite3_bind_blob(insert.get(), 6, vectors[i].data(), static_cast<int>(vectors[i].size() * sizeof(float)), SQLITE_TRANSIENT) != SQLITE_OK)
                throw std::runtime_error("Не удалось сохранить эмбеддинг.");
            sqlite3_bind_int(insert.get(), 7, chunks[i].page);
            sqlite3_bind_int(insert.get(), 8, chunks[i].lineStart);
            sqlite3_bind_int(insert.get(), 9, chunks[i].lineEnd);
            if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        }
        exec(db.get(), "COMMIT;");
    } catch (...) {
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    if (progress) progress(chunks.size(), chunks.size());
    return chunks.size();
}
