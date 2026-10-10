#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct DocumentPart {
    std::string text;
    int page = 0;
};
struct DocumentChunk {
    std::string text;
    int page = 0, lineStart = 0, lineEnd = 0;
};
std::vector<DocumentPart> extractDocument(const std::filesystem::path& file,
                                         const std::filesystem::path& applicationDirectory);
std::vector<DocumentChunk> chunkDocument(const std::vector<DocumentPart>& parts);
using EmbeddingFunction = std::function<std::vector<float>(const std::string&)>;
using IndexProgress = std::function<void(size_t, size_t)>;
size_t indexDocument(const std::filesystem::path& database, const std::filesystem::path& file,
                     const std::filesystem::path& applicationDirectory,
                     const EmbeddingFunction& embed, const IndexProgress& progress = {});
