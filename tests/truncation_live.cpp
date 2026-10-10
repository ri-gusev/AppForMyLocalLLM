#include "rag_chat.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    try {
        RagChat chat(std::filesystem::path("unused-rag-directory"));
        auto reply = chat.ask("Напечатай все целые числа от 1 до 3000 по порядку, каждое на отдельной строке. Без пояснений, сокращений и многоточий.", false);
        if (!reply.generation.truncated || reply.generation.outputTokens > 512 ||
            reply.displayText().find("Ответ обрезан") == std::string::npos)
            throw std::runtime_error("Live output limit was not exercised");
        nlohmann::json report = {{"output_tokens", reply.generation.outputTokens},
            {"input_tokens", reply.generation.inputTokens}, {"truncated", reply.generation.truncated},
            {"display_text", reply.displayText()}};
        std::ofstream(argv[1], std::ios::binary) << report.dump(2) << '\n';
        std::cout << "PASS: actual Ollama output limit and visible truncation warning\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
