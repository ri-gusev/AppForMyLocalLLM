#include "ollama_client.h"
#include <windows.h>
#include <iostream>

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    OllamaClient client;
    std::cout << "Chat with qwen3:8b. Press Ctrl+C to exit.\n";
    while (true) {
        std::cout << "Prompt: " << std::flush;
        std::string prompt;
        if (!std::getline(std::cin, prompt)) break;
        if (prompt.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            std::cout << client.chat(prompt) << "\n\n";
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
        }
    }
    return 0;
}
