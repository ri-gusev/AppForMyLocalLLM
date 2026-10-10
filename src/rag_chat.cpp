#include "rag_chat.h"
#include "rag_context.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
long long elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
struct DbCloser { void operator()(sqlite3* db) const { sqlite3_close(db); } };
struct StatementCloser { void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); } };
std::string column(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}
std::string lower(std::string text) {
    for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}
bool wordMatch(const std::string& text, const std::string& word) {
    const auto identifier = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    };
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
        if ((at == 0 || !identifier(text[at - 1])) &&
            (at + word.size() == text.size() || !identifier(text[at + word.size()]))) return true;
    return false;
}
// The same bounded lexical bonus used by the source project's rag_retrieval.cpp.
void rerank(const std::string& question, std::vector<RagHit>& hits) {
    const std::set<std::string> stop = {"the","and","with","what","where","which","does","how",
        "from","for","that","this","code","document","documents"};
    const std::regex identifiers(R"([A-Za-z_][A-Za-z0-9_:.\-]*)");
    std::set<std::string> terms, entities;
    for (std::sregex_iterator it(question.begin(), question.end(), identifiers), end; it != end; ++it) {
        auto token = it->str();
        auto normalized = lower(token);
        if (normalized.size() < 3 || stop.count(normalized)) continue;
        terms.insert(normalized);
        if (token.find('_') != token.npos || token.find("::") != token.npos ||
            std::any_of(token.begin() + 1, token.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; }))
            entities.insert(token);
    }
    for (auto& hit : hits) {
        auto file = lower(hit.file), section = lower(hit.section), content = lower(hit.content);
        bool fileMatch = false, sectionMatch = false, entityMatch = false;
        int matched = 0;
        for (const auto& term : terms) {
            fileMatch = fileMatch || file.find(term) != file.npos;
            sectionMatch = sectionMatch || wordMatch(section, term);
            if (wordMatch(content, term)) ++matched;
        }
        for (const auto& entity : entities)
            entityMatch = entityMatch || wordMatch(hit.section, entity) || wordMatch(hit.content, entity);
        double bonus = std::min(0.12, (fileMatch ? 0.04 : 0) + (sectionMatch ? 0.05 : 0) +
            std::min(0.03, matched * 0.01) + (entityMatch ? 0.04 : 0));
        hit.relevance = std::min(1.0, hit.score + bonus);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.relevance > b.relevance; });
}
}

RagChat::RagChat(std::filesystem::path baseDirectory, double temperature) : base_(std::move(baseDirectory)), model_("qwen3:8b", temperature) {}

size_t RagChat::addFile(const std::filesystem::path& file, const IndexProgress& progress) {
    return indexDocument(base_ / "data/user_index.db", file, base_, OllamaClient::embed, progress);
}

std::vector<RagHit> RagChat::retrieve(const std::string& question) const {
    std::ifstream configFile(base_ / "config/rag_config.json");
    if (!configFile) throw std::runtime_error("RAG config missing beside the application: config/rag_config.json");
    const auto config = nlohmann::json::parse(configFile);
    const double preNumber = config.at("pre_filter_top_k").get<double>();
    const double postNumber = config.at("post_filter_top_k").get<double>();
    const double threshold = config.at("similarity_threshold").get<double>();
    if (!std::isfinite(preNumber) || !std::isfinite(postNumber) || !std::isfinite(threshold) ||
        preNumber < 1 || preNumber > 20 || std::floor(preNumber) != preNumber ||
        postNumber < 1 || postNumber > preNumber || std::floor(postNumber) != postNumber || threshold < 0 || threshold > 1)
        throw std::runtime_error("Invalid RAG config: top_k 1..20, post <= pre, threshold 0..1");
    const int pre = static_cast<int>(preNumber), post = static_cast<int>(postNumber);
    const auto seed = base_ / "data/document_index.db";
    const auto uploaded = base_ / "data/user_index.db";
    std::vector<std::filesystem::path> databases;
    if (std::filesystem::exists(seed)) databases.push_back(seed);
    if (std::filesystem::exists(uploaded)) databases.push_back(uploaded);
    if (databases.empty()) throw std::runtime_error("Cannot open local RAG index: no index found");
    std::vector<float> query;
    double queryNorm = 0;
    std::vector<RagHit> hits;
    for (const auto& database : databases) {
    sqlite3* rawDb = nullptr;
    int rc = sqlite3_open_v2(database.u8string().c_str(), &rawDb,
                            SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, DbCloser> db(rawDb);
    if (rc != SQLITE_OK) throw std::runtime_error("Cannot open local RAG index: " + std::string(sqlite3_errmsg(db.get())));
    sqlite3_stmt* rawStatement = nullptr;
    rc = sqlite3_prepare_v2(db.get(),
        "SELECT file,title,section,chunk_id,content,embedding,embedding_dim,page,line_start,line_end "
        "FROM document_chunks WHERE strategy IN ('structural','semantic') ORDER BY id", -1, &rawStatement, nullptr);
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(rawStatement);
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.get()));
    if (query.empty()) {
        query = OllamaClient::embed(question);
        for (float value : query) queryNorm += static_cast<double>(value) * value;
    }
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        int dim = sqlite3_column_int(statement.get(), 6);
        int bytes = sqlite3_column_bytes(statement.get(), 5);
        if (dim <= 0 || static_cast<size_t>(dim) != query.size() ||
            static_cast<size_t>(bytes) != query.size() * sizeof(float))
            throw std::runtime_error("Index embeddings incompatible with bge-m3; rebuild the source index");
        std::vector<float> embedding(dim);
        std::memcpy(embedding.data(), sqlite3_column_blob(statement.get(), 5), bytes);
        double dot = 0, norm = 0;
        for (size_t i = 0; i < query.size(); ++i) {
            if (!std::isfinite(embedding[i])) throw std::runtime_error("Invalid index embedding");
            dot += static_cast<double>(query[i]) * embedding[i];
            norm += static_cast<double>(embedding[i]) * embedding[i];
        }
        if (norm == 0) throw std::runtime_error("Zero vector in index");
        RagHit hit;
        hit.file = column(statement.get(), 0); hit.title = column(statement.get(), 1);
        hit.section = column(statement.get(), 2); hit.chunkId = column(statement.get(), 3);
        hit.content = column(statement.get(), 4); hit.page = sqlite3_column_int(statement.get(), 7);
        hit.lineStart = sqlite3_column_int(statement.get(), 8); hit.lineEnd = sqlite3_column_int(statement.get(), 9);
        hit.score = dot / std::sqrt(queryNorm * norm);
        hits.push_back(std::move(hit));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
    }
    if (hits.empty()) throw std::runtime_error("Local RAG index contains no document chunks");
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    hits = deduplicateSources(std::move(hits));
    if (hits.size() > static_cast<size_t>(pre)) hits.resize(pre);
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const auto& hit) { return hit.score < threshold; }), hits.end());
    rerank(question, hits);
    if (hits.size() > static_cast<size_t>(post)) hits.resize(post);
    return hits;
}

RagReply RagChat::ask(const std::string& question, bool useRag) {
    if (question.find_first_not_of(" \t\r\n") == std::string::npos) throw std::runtime_error("Empty question");
    RagReply reply;
    auto start = Clock::now();
    reply.usedRag = useRag;
    if (!useRag) {
        reply.answer = model_.chat(question);
        reply.generation = model_.lastStats();
        reply.generationMs = elapsed(start);
        return reply;
    }
    generation::contextByteBudget(question);
    reply.sources = retrieve(question);
    reply.retrievalMs = elapsed(start);
    if (reply.sources.empty()) {
        reply.answer = "В локальном индексе не найдено достаточно подходящих фрагментов для ответа.";
        return reply;
    }
    auto context = prepareRagContext(question, reply.sources);
    reply.sources = std::move(context.sources);
    reply.contextReduced = context.reduced;
    if (reply.sources.empty()) throw std::runtime_error("Документы не помещаются в контекст. Сократите вопрос.");
    start = Clock::now();
    reply.answer = model_.chat(question, context.json);
    reply.answer = normalizeCitations(reply.answer, reply.sources.size(), reply.invalidCitations);
    reply.generation = model_.lastStats();
    reply.generationMs = elapsed(start);
    return reply;
}

std::string RagReply::displayText() const {
    std::ostringstream text;
    text << answer;
    if (generation.truncated) text << "\n\n[Ответ обрезан: достигнут лимит 512 токенов. Уточните вопрос или разбейте его на части.]";
    if (invalidCitations) text << "\n\n[В ответе есть ссылка на неизвестный источник. Проверьте ответ.]";
    if (contextReduced) text << "\n\n[Контекст документов сокращён под размер окна.]";
    if (generation.droppedHistory) text << "\n[Часть ранней истории не включена в этот запрос.]";
    if (!sources.empty()) {
        text << "\n\nИсточники (фрагменты, переданные модели):";
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& hit = sources[i];
            text << "\n[" << i + 1 << "] " << hit.file;
            if (hit.lineStart > 0) text << ":" << hit.lineStart << "–" << hit.lineEnd;
            if (hit.page > 0) text << " (страница " << hit.page << ")";
            if (!hit.section.empty()) text << " — " << hit.section;
        }
    }
    if (usedRag) text << "\n\nПоиск: " << retrievalMs << " мс · Ответ: " << generationMs << " мс";
    else text << "\n\nОтвет: " << generationMs << " мс";
    return text.str();
}
