#include "../src/chat_window.cpp"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void awaitResponse(HWND window, WindowState& state) {
    auto start = GetTickCount64();
    while (state.busy && GetTickCount64() - start < 180000) {
        SendMessageW(window, WM_TIMER, 1, 0);
        Sleep(20);
    }
    require(!state.busy && !state.conversation->failed, "Plain chat request failed");
    require(IsWindowEnabled(state.ragToggle), "Toggle not restored after response");
}
int main() {
    try {
        // There is intentionally no RAG configuration or database in this directory.
        auto root = std::filesystem::temp_directory_path() / ("localagent-modes-" + std::to_string(GetCurrentProcessId()));
        require(!std::filesystem::exists(root), "Test directory unexpectedly exists");
        auto instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"ChatModesTest";
        require(RegisterClassW(&wc), "RegisterClass failed");
        WindowState state(root);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"Modes test", WS_OVERLAPPEDWINDOW,
            0, 0, 1040, 820, nullptr, nullptr, instance, &state);
        require(window && state.ragToggle && state.ragEnabled, "RAG toggle missing or incorrect default");
        auto toggle = [&] { SendMessageW(window, WM_COMMAND, MAKEWPARAM(ragToggleId, BN_CLICKED), 0); };
        toggle();
        require(!state.ragEnabled, "Toggle did not disable RAG");
        SetWindowTextW(state.input, L"Запомни кодовое слово SAPPHIRE. Ответь кратко.");
        SendMessageW(window, submitMessage, 0, 0);
        require(state.busy && !IsWindowEnabled(state.ragToggle), "Toggle not locked during request");
        toggle();
        require(!state.ragEnabled, "Busy toggle changed request mode");
        awaitResponse(window, state);
        require(state.conversation->result.find("Поиск:") == std::string::npos &&
                state.conversation->result.find("Источники") == std::string::npos,
                "Plain answer includes RAG metadata");
        auto second = state.conversation->client.ask("Какое кодовое слово я назвал? Ответь только им.", false);
        require(!second.usedRag && second.sources.empty() && second.retrievalMs == 0, "Plain reply used RAG");
        require(second.answer.find("SAPPHIRE") != std::string::npos || second.answer.find("Sapphire") != std::string::npos,
                "Plain conversation history missing");
        toggle();
        require(state.ragEnabled, "Toggle did not restore RAG");
        bool expectedError = false;
        try { state.conversation->client.ask("Test RAG mode", true); }
        catch (const std::exception& error) {
            expectedError = std::string(error.what()).find("config missing") != std::string::npos;
        }
        require(expectedError, "Enabled RAG did not use retrieval pipeline");
        require(!std::filesystem::exists(root), "Plain chat created a RAG database");
        DestroyWindow(window);
        DeleteObject(state.font);
        std::cout << "PASS: toggle modes, busy lock, plain chat without RAG assets, history, RAG pipeline restored\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
