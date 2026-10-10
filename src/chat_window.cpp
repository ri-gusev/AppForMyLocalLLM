#include "chat_window.h"
#include "rag_chat.h"
#include <commctrl.h>
#include <commdlg.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr int sendId = 100;
constexpr int addFileId = 101;
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
    explicit Conversation(const std::filesystem::path& base) : client(base) {}
    RagChat client;
    std::mutex mutex;
    bool ready = false, failed = false;
    std::string result, progress;
};
struct WindowState {
    explicit WindowState(const std::filesystem::path& base) : conversation(std::make_shared<Conversation>(base)) {}
    HWND history = nullptr, input = nullptr, send = nullptr, status = nullptr, addFile = nullptr;
    HFONT font = nullptr;
    bool busy = false;
    bool importing = false;
    std::shared_ptr<Conversation> conversation;
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
        try { result = conversation->client.ask(text).displayText(); }
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
    EnableWindow(state.addFile, FALSE);
    SetWindowTextW(state.status, L"Поиск по документам и ответ модели…");
}
void importFiles(WindowState& state, std::vector<std::filesystem::path> files) {
    if (state.busy || files.empty()) return;
    std::thread([conversation = state.conversation, files = std::move(files)] {
        std::string result;
        for (size_t i = 0; i < files.size(); ++i) {
            const auto name = files[i].filename().u8string();
            try {
                {
                    std::lock_guard<std::mutex> lock(conversation->mutex);
                    conversation->progress = "Чтение файла " + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ": " + name;
                }
                size_t count = conversation->client.addFile(files[i], [&](size_t done, size_t total) {
                    std::lock_guard<std::mutex> lock(conversation->mutex);
                    conversation->progress = name + ": индексация " + std::to_string(done) + "/" + std::to_string(total);
                });
                result += name + ": проиндексировано фрагментов — " + std::to_string(count) + "\n";
            } catch (const std::exception& error) {
                result += name + ": ошибка — " + error.what() + "\n";
            }
        }
        std::lock_guard<std::mutex> lock(conversation->mutex);
        conversation->result = std::move(result);
        conversation->failed = false;
        conversation->progress.clear();
        conversation->ready = true;
    }).detach();
    state.busy = true;
    state.importing = true;
    EnableWindow(state.send, FALSE);
    EnableWindow(state.addFile, FALSE);
    SetWindowTextW(state.status, L"Чтение и индексация файлов…");
}
void chooseFiles(HWND window, WindowState& state) {
    if (state.busy) return;
    std::vector<wchar_t> selection(65536, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Документы (TXT, MD, PDF, DOCX)\0*.txt;*.md;*.pdf;*.docx\0\0";
    dialog.lpstrFile = selection.data();
    dialog.nMaxFile = static_cast<DWORD>(selection.size());
    dialog.lpstrTitle = L"Добавить файлы в семантический индекс";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) append(state.history, L"Ошибка выбора файлов. Попробуйте выбрать меньше файлов.\r\n\r\n");
        return;
    }
    std::filesystem::path first(selection.data());
    auto* next = selection.data() + first.wstring().size() + 1;
    std::vector<std::filesystem::path> files;
    if (!*next) files.push_back(first);
    else while (*next) {
        std::wstring name(next);
        files.push_back(first / name);
        next += name.size() + 1;
    }
    importFiles(state, std::move(files));
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
        state->addFile = CreateWindowExW(0, L"BUTTON", L"Добавить файлы", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(addFileId), instance, nullptr);
        state->status = CreateWindowExW(0, L"STATIC", L"qwen3:8b · Enter — отправить · Shift+Enter — новая строка",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        if (!state->history || !state->input || !state->send || !state->status || !state->addFile) return -1;
        for (HWND control : {state->history, state->input, state->send, state->status, state->addFile})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->history, EM_SETLIMITTEXT, 0, 0);
        SendMessageW(state->input, EM_SETLIMITTEXT, 65536, 0);
        if (!SetWindowSubclass(state->input, inputProc, 1, 0) || !SetTimer(window, 1, 100, nullptr)) return -1;
        return 0;
    }
    case WM_SIZE:
        if (state) {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->addFile, 12, 12, 170, 32, TRUE);
            MoveWindow(state->history, 12, 56, width - 24, height - 188, TRUE);
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
        if (LOWORD(wParam) == addFileId && HIWORD(wParam) == BN_CLICKED && state) {
            try { chooseFiles(window, *state); }
            catch (const std::exception& error) {
                append(state->history, L"Ошибка: " + fromUtf8(error.what()) + L"\r\n\r\n");
            }
            return 0;
        }
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
                append(state->history, (state->importing ? L"Индексация:\r\n" :
                    state->conversation->failed ? L"Ошибка:\r\n" : L"Qwen:\r\n") +
                    fromUtf8(state->conversation->result) + L"\r\n\r\n");
                state->conversation->ready = false;
                state->busy = false;
                state->importing = false;
                EnableWindow(state->send, TRUE);
                EnableWindow(state->addFile, TRUE);
                SetWindowTextW(state->status, L"qwen3:8b · Enter — отправить · Shift+Enter — новая строка");
            } else if (!state->conversation->progress.empty()) {
                SetWindowTextW(state->status, fromUtf8(state->conversation->progress).c_str());
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
    std::wstring executable(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return 1;
    executable.resize(length);
    WindowState state(std::filesystem::path(executable).parent_path());
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
