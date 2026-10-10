#include "chat_window.h"
#include "ollama_client.h"
#include <commctrl.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr int sendId = 100;
constexpr UINT submitMessage = WM_APP + 1;

std::wstring fromUtf8(const std::string& text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string toUtf8(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
struct Conversation {
    OllamaClient client;
    std::mutex mutex;
    bool ready = false, failed = false;
    std::string result;
};
struct WindowState {
    HWND history = nullptr, input = nullptr, send = nullptr, status = nullptr;
    HFONT font = nullptr;
    bool busy = false;
    std::shared_ptr<Conversation> conversation = std::make_shared<Conversation>();
};
void append(HWND history, const std::wstring& text) {
    std::wstring normalized;
    for (wchar_t ch : text) {
        if (ch == L'\n' && (normalized.empty() || normalized.back() != L'\r')) normalized += L'\r';
        normalized += ch;
    }
    SendMessageW(history, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(history, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
    SendMessageW(history, EM_SCROLLCARET, 0, 0);
}
LRESULT CALLBACK inputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        SendMessageW(GetParent(window), submitMessage, 0, 0);
        return 0;
    }
    if (message == WM_CHAR && wParam == L'\r' && !(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    return DefSubclassProc(window, message, wParam, lParam);
}
void send(WindowState& state) {
    if (state.busy) return;
    int length = GetWindowTextLengthW(state.input);
    std::wstring prompt(length + 1, L'\0');
    GetWindowTextW(state.input, prompt.data(), length + 1);
    prompt.resize(length);
    if (prompt.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
    // The worker owns shared state and never accesses window handles.
    std::thread([conversation = state.conversation, text = toUtf8(prompt)] {
        std::string result;
        bool failed = false;
        try { result = conversation->client.chat(text); }
        catch (const std::exception& error) { result = error.what(); failed = true; }
        std::lock_guard<std::mutex> lock(conversation->mutex);
        conversation->result = std::move(result);
        conversation->failed = failed;
        conversation->ready = true;
    }).detach();
    state.busy = true;
    append(state.history, L"Вы:\r\n" + prompt + L"\r\n\r\n");
    SetWindowTextW(state.input, L"");
    EnableWindow(state.send, FALSE);
    SetWindowTextW(state.status, L"Модель отвечает…");
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        state->font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        state->history = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
            WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        state->input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        state->send = CreateWindowExW(0, L"BUTTON", L"Отправить", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(sendId), instance, nullptr);
        state->status = CreateWindowExW(0, L"STATIC", L"qwen3:8b · Enter — отправить · Shift+Enter — новая строка",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        if (!state->history || !state->input || !state->send || !state->status) return -1;
        for (HWND control : {state->history, state->input, state->send, state->status})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->history, EM_SETLIMITTEXT, 0, 0);
        SendMessageW(state->input, EM_SETLIMITTEXT, 65536, 0);
        if (!SetWindowSubclass(state->input, inputProc, 1, 0) || !SetTimer(window, 1, 100, nullptr)) return -1;
        return 0;
    }
    case WM_SIZE:
        if (state) {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->history, 12, 12, width - 24, height - 144, TRUE);
            MoveWindow(state->status, 12, height - 123, width - 24, 24, TRUE);
            MoveWindow(state->input, 12, height - 92, width - 156, 80, TRUE);
            MoveWindow(state->send, width - 132, height - 92, 120, 80, TRUE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {620, 420};
        return 0;
    case WM_SETFOCUS:
        if (state) SetFocus(state->input);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) != sendId || HIWORD(wParam) != BN_CLICKED) break;
        [[fallthrough]];
    case submitMessage:
        if (state) {
            try { send(*state); }
            catch (const std::exception& error) {
                append(state->history, L"Ошибка: " + fromUtf8(error.what()) + L"\r\n\r\n");
            }
        }
        return 0;
    case WM_TIMER:
        if (state && state->busy) {
            std::lock_guard<std::mutex> lock(state->conversation->mutex);
            if (state->conversation->ready) {
                append(state->history, (state->conversation->failed ? L"Ошибка:\r\n" : L"Qwen:\r\n") +
                    fromUtf8(state->conversation->result) + L"\r\n\r\n");
                state->conversation->ready = false;
                state->busy = false;
                EnableWindow(state->send, TRUE);
                SetWindowTextW(state->status, L"qwen3:8b · Enter — отправить · Shift+Enter — новая строка");
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
int runChatWindow(HINSTANCE instance, int showCommand) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"LocalAgentChat";
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&windowClass)) return 1;
    WindowState state;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"LocalAgent — Qwen", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 650, nullptr, nullptr, instance, &state);
    if (!window) {
        if (state.font) DeleteObject(state.font);
        MessageBoxW(nullptr, L"Не удалось создать окно чата.", L"LocalAgent", MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, showCommand);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.font) DeleteObject(state.font);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
