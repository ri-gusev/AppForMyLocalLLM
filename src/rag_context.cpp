#include "rag_context.h"
#include "generation_policy.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <regex>

namespace {
std::string normalize(const std::string& text) {
    std::string result;
    bool space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) { space = !result.empty(); continue; }
        if (space) result += ' ';
        result += static_cast<char>(ch);
        space = false;
    }
    return result;
}
size_t utf8End(const std::string& text, size_t limit) {
    size_t end = std::min(text.size(), limit);
    while (end < text.size() && end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return end;
}
}
std::vector<RagHit> deduplicateSources(std::vector<RagHit> hits) {
    std::vector<RagHit> unique;
    std::vector<std::string> texts;
    for (auto& hit : hits) {
        auto text = normalize(hit.content);
        bool duplicate = false;
        for (size_t i = 0; i < texts.size(); ++i) {
            if (text == texts[i]) { duplicate = true; break; }
            // Keep similar passages with different facts/numbers; remove only contained evidence.
            if (text.size() >= 256 && texts[i].find(text) != std::string::npos) {
                duplicate = true; break;
            }
        }
        if (!duplicate) {
            texts.push_back(std::move(text));
            unique.push_back(std::move(hit));
        }
    }
    return unique;
}
PreparedRagContext prepareRagContext(const std::string& question, const std::vector<RagHit>& hits) {
    const size_t budget = generation::contextByteBudget(question);
    PreparedRagContext result;
    auto json = nlohmann::json::array();
    for (size_t i = 0; i < hits.size(); ++i) {
        auto hit = hits[i];
        const size_t originalSize = hit.content.size();
        // Remove repeated boundary text from overlapping chunks of the same file/page.
        for (const auto& previous : result.sources) {
            if (previous.file != hit.file || previous.page != hit.page) continue;
            size_t maximum = std::min({previous.content.size(), hit.content.size(), size_t(700)});
            for (size_t overlap = maximum; overlap >= 80; --overlap) {
                if (previous.content.compare(previous.content.size() - overlap, overlap, hit.content, 0, overlap) == 0) {
                    if (overlap < hit.content.size() && (static_cast<unsigned char>(hit.content[overlap]) & 0xc0) == 0x80) continue;
                    hit.lineStart += static_cast<int>(std::count(hit.content.begin(), hit.content.begin() + overlap, '\n'));
                    hit.content.erase(0, overlap);
                    result.reduced = true;
                    break;
                }
            }
        }
        const size_t remaining = budget > json.dump().size() ? budget - json.dump().size() : 0;
        const size_t share = remaining / (hits.size() - i);
        size_t end = utf8End(hit.content, std::min<size_t>(2200, share));
        nlohmann::json item;
        bool fits = false;
        while (end >= 160 || end == hit.content.size()) {
            item = {{"id", result.sources.size() + 1}, {"file", hit.file}, {"section", hit.section},
                {"chunk_id", hit.chunkId}, {"page", hit.page}, {"line_start", hit.lineStart},
                {"line_end", hit.lineEnd}, {"content", hit.content.substr(0, end)}, {"partial", end < originalSize}};
            if (item.dump().size() + 1 <= share && json.dump().size() + item.dump().size() + 1 <= budget) { fits = true; break; }
            if (end == 0) break;
            end = utf8End(hit.content, end > 64 ? end - 64 : 0);
        }
        if (!fits || end == 0) { result.reduced = true; continue; }
        if (end < hit.content.size()) {
            const size_t newline = hit.content.rfind('\n', end - 1);
            if (newline != hit.content.npos && newline >= end / 2) end = newline + 1;
            item["content"] = hit.content.substr(0, end);
            item["partial"] = true;
            result.reduced = true;
        }
        hit.content.resize(end);
        if (hit.lineStart > 0) {
            hit.lineEnd = hit.lineStart + static_cast<int>(std::count(hit.content.begin(), hit.content.end(), '\n'));
            if (!hit.content.empty() && hit.content.back() == '\n') --hit.lineEnd;
            item["line_end"] = hit.lineEnd;
        }
        json.push_back(std::move(item));
        result.sources.push_back(std::move(hit));
    }
    result.json = result.sources.empty() ? "" : json.dump();
    return result;
}

std::string normalizeCitations(const std::string& answer, size_t sourceCount, bool& invalid) {
    invalid = false;
    const std::regex citations(R"(\[(?:id\s*:?\s*)?([0-9]+)\])", std::regex::icase);
    std::vector<bool> code(answer.size(), false);
    size_t fence = 0;
    for (size_t i = 0; i < answer.size();) {
        if (answer[i] == '`') {
            size_t end = i;
            while (end < answer.size() && answer[end] == '`') ++end;
            const size_t count = end - i;
            for (size_t j = i; j < end; ++j) code[j] = true;
            if (!fence) fence = count;
            else if (fence == count) fence = 0;
            i = end;
        } else { code[i] = fence != 0; ++i; }
    }
    std::string result;
    size_t previous = 0;
    for (std::sregex_iterator it(answer.begin(), answer.end(), citations), end; it != end; ++it) {
        result += answer.substr(previous, static_cast<size_t>(it->position()) - previous);
        if (code[static_cast<size_t>(it->position())]) {
            result += it->str();
            previous = static_cast<size_t>(it->position() + it->length());
            continue;
        }
        size_t number = 0;
        try { number = std::stoull((*it)[1].str()); } catch (...) { invalid = true; }
        if (number > 0 && number <= sourceCount) result += "[" + std::to_string(number) + "]";
        else { result += it->str(); invalid = true; }
        previous = static_cast<size_t>(it->position() + it->length());
    }
    return result + answer.substr(previous);
}
