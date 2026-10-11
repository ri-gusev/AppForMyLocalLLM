#include "chat_window.h"
#include "rag_chat.h"
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int sendId = 100;
constexpr int addFileId = 101;
constexpr int ragToggleId = 102;
constexpr UINT submitMessage = WM_APP + 1;
namespace palette {
constexpr COLORREF background = RGB(17, 13, 27);
constexpr COLORREF panel = RGB(29, 22, 43);
constexpr COLORREF input = RGB(38, 28, 54);
constexpr COLORREF border = RGB(70, 50, 95);
constexpr COLORREF purple = RGB(186, 151, 255);
constexpr COLORREF green = RGB(155, 231, 158);
constexpr COLORREF text = RGB(238, 230, 249);
constexpr COLORREF muted = RGB(168, 150, 189);
}

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
    HWND history = nullptr, input = nullptr, send = nullptr, status = nullptr, addFile = nullptr, ragToggle = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr, smallFont = nullptr, buttonFont = nullptr;
    HBRUSH backgroundBrush = CreateSolidBrush(palette::background);
    HBRUSH panelBrush = CreateSolidBrush(palette::panel);
    HBRUSH inputBrush = CreateSolidBrush(palette::input);
    int dpi = 96;
    HWND hoveredButton = nullptr;
    int wheelRemainder = 0;
    bool busy = false;
    bool importing = false;
    bool ragEnabled = true;
    std::shared_ptr<Conversation> conversation;
    int px(int value) const { return MulDiv(value, dpi, 96); }
    ~WindowState() {
        for (auto brush : {backgroundBrush, panelBrush, inputBrush}) if (brush) DeleteObject(brush);
        for (auto extraFont : {titleFont, smallFont, buttonFont}) if (extraFont) DeleteObject(extraFont);
    }
};
void readyStatus(WindowState& state) {
    SetWindowTextW(state.status, state.ragEnabled ? L"●  qwen3:8b · Поиск по документам включён" :
                                                   L"●  qwen3:8b · Обычный чат без RAG");
}
void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    auto brush = CreateSolidBrush(fill);
    auto pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}
void label(HDC dc, HFONT font, COLORREF color, RECT rect, const wchar_t* text, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags);
    SelectObject(dc, previous);
}
void paintWindow(HWND window, HDC dc, WindowState& state) {
    RECT area;
    GetClientRect(window, &area);
    FillRect(dc, &area, state.backgroundBrush);
    const auto p = [&](int value) { return state.px(value); };
    const int width = area.right, height = area.bottom;
    rounded(dc, {p(24), p(27), p(68), p(71)}, palette::input, palette::border, p(14));
    label(dc, state.titleFont, palette::green, {p(24), p(27), p(68), p(71)}, L"Q", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    label(dc, state.titleFont, palette::text, {p(84), p(22), width - p(230), p(58)}, L"LocalAgent");
    label(dc, state.smallFont, palette::muted, {p(85), p(58), width - p(230), p(80)}, L"Ваш локальный AI · Qwen 3");
    RECT pill{width - p(208), p(32), width - p(24), p(64)};
    rounded(dc, pill, RGB(26, 47, 37), RGB(54, 84, 60), p(24));
    label(dc, state.smallFont, palette::green, pill, L"ЛОКАЛЬНАЯ МОДЕЛЬ", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    label(dc, state.smallFont, palette::purple, {p(26), p(103), width - p(230), p(137)}, L"ДИАЛОГ");
    rounded(dc, {p(24), p(148), width - p(24), height - p(204)}, palette::panel, palette::border, p(18));
    rounded(dc, {p(24), height - p(148), width - p(24), height - p(44)}, palette::input, palette::border, p(18));
    label(dc, state.smallFont, palette::muted, {p(40), height - p(140), width - p(230), height - p(117)}, L"ВАШЕ СООБЩЕНИЕ");
    label(dc, state.smallFont, palette::muted, {p(27), height - p(36), width - p(24), height - p(8)},
          L"Enter — отправить   ·   Shift+Enter — новая строка");
    if (GetWindowTextLengthW(state.history) == 0) {
        const int center = (p(148) + height - p(204)) / 2;
        label(dc, state.titleFont, palette::purple, {p(60), center - p(45), width - p(60), center},
              L"О чём поговорим?", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        label(dc, state.font, palette::muted, {p(60), center + p(5), width - p(60), center + p(35)},
              state.ragEnabled ? L"Добавьте документы и задайте свой вопрос" : L"Задайте вопрос вашей локальной модели",
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}
LRESULT CALLBACK buttonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    if (state && message == WM_MOUSEMOVE && state->hoveredButton != window) {
        state->hoveredButton = window;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    }
    if (state && message == WM_MOUSELEAVE) {
        state->hoveredButton = nullptr;
        InvalidateRect(window, nullptr, FALSE);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
LRESULT CALLBACK historyProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL) {
        auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        if (state) {
            state->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            const int steps = state->wheelRemainder / WHEEL_DELTA;
            state->wheelRemainder %= WHEEL_DELTA;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) {
                for (int i = 0; i < std::abs(steps); ++i)
                    SendMessageW(window, EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            } else {
                SendMessageW(window, EM_LINESCROLL, 0, -steps * static_cast<int>(lines));
            }
            return 0;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
void append(HWND history, const std::wstring& text) {
    std::wstring normalized;
    for (wchar_t ch : text) {
        if (ch == L'\n' && (normalized.empty() || normalized.back() != L'\r')) normalized += L'\r';
        normalized += ch;
    }
    SendMessageW(history, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BOLD;
    format.crTextColor = palette::text;
    const size_t heading = normalized.find(L"\r\n");
    if (heading != normalized.npos && heading < 32) {
        format.dwEffects = CFE_BOLD;
        format.crTextColor = normalized.find(L"Вы:") == 0 ? palette::green : palette::purple;
        SendMessageW(history, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        auto prefix = normalized.substr(0, heading);
        SendMessageW(history, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(prefix.c_str()));
        normalized.erase(0, heading);
    }
    format.dwEffects = 0;
    format.crTextColor = palette::text;
    SendMessageW(history, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    SendMessageW(history, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
    SendMessageW(history, EM_SCROLLCARET, 0, 0);
    ShowWindow(history, SW_SHOW);
    InvalidateRect(GetParent(history), nullptr, FALSE);
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
    std::thread([conversation = state.conversation, text = toUtf8(prompt), useRag = state.ragEnabled] {
        std::string result;
        bool failed = false;
        try { result = conversation->client.ask(text, useRag).displayText(); }
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
    EnableWindow(state.ragToggle, FALSE);
    SetWindowTextW(state.status, state.ragEnabled ? L"Поиск по документам и ответ модели…" : L"Модель отвечает…");
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
    EnableWindow(state.ragToggle, FALSE);
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
        HDC screen = GetDC(window);
        state->dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(window, screen);
        state->font = CreateFontW(-state->px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        state->titleFont = CreateFontW(-state->px(27), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        state->smallFont = CreateFontW(-state->px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        state->buttonFont = CreateFontW(-state->px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        static const HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
        if (!richEdit) return -1;
        state->history = CreateWindowExW(0, L"RICHEDIT50W", L"", WS_CHILD |
            WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        state->input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        state->send = CreateWindowExW(0, L"BUTTON", L"Отправить  →", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_OWNERDRAW, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(sendId), instance, nullptr);
        state->addFile = CreateWindowExW(0, L"BUTTON", L"Добавить файлы", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_OWNERDRAW, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(addFileId), instance, nullptr);
        state->ragToggle = CreateWindowExW(0, L"BUTTON", L"RAG включён", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_OWNERDRAW, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(ragToggleId), instance, nullptr);
        state->status = CreateWindowExW(0, L"STATIC", L"●  qwen3:8b · Готово к сообщению",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        if (!state->history || !state->input || !state->send || !state->status || !state->addFile || !state->ragToggle) return -1;
        readyStatus(*state);
        for (HWND control : {state->history, state->input, state->send, state->status, state->addFile})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->send, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
        SendMessageW(state->addFile, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
        SendMessageW(state->status, WM_SETFONT, reinterpret_cast<WPARAM>(state->smallFont), TRUE);
        SendMessageW(state->ragToggle, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
        SendMessageW(state->history, EM_SETBKGNDCOLOR, 0, palette::panel);
        SetWindowTheme(state->history, L"DarkMode_Explorer", nullptr);
        SendMessageW(state->history, EM_EXLIMITTEXT, 0, 0x7fffffff);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        format.crTextColor = palette::text;
        format.yHeight = 220;
        lstrcpyW(format.szFaceName, L"Segoe UI");
        SendMessageW(state->history, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        SendMessageW(state->input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SendMessageW(state->input, EM_SETLIMITTEXT, 65536, 0);
        if (!SetWindowSubclass(state->input, inputProc, 1, 0) ||
            !SetWindowSubclass(state->history, historyProc, 1, 0) ||
            !SetWindowSubclass(state->send, buttonProc, 1, 0) ||
            !SetWindowSubclass(state->ragToggle, buttonProc, 1, 0) ||
            !SetWindowSubclass(state->addFile, buttonProc, 1, 0) || !SetTimer(window, 1, 100, nullptr)) return -1;
        return 0;
    }
    case WM_SIZE:
        if (state) {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            const auto p = [&](int value) { return state->px(value); };
            MoveWindow(state->addFile, width - p(218), p(103), p(194), p(34), TRUE);
            MoveWindow(state->ragToggle, width - p(396), p(103), p(162), p(34), TRUE);
            MoveWindow(state->history, p(40), p(166), width - p(80), std::max(p(20), height - p(390)), TRUE);
            MoveWindow(state->status, p(28), height - p(190), width - p(56), p(28), TRUE);
            MoveWindow(state->input, p(40), height - p(113), width - p(250), p(52), TRUE);
            MoveWindow(state->send, width - p(190), height - p(119), p(150), p(46), TRUE);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {state ? state->px(760) : 760, state ? state->px(620) : 620};
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        if (state) paintWindow(window, dc, *state);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (state) paintWindow(window, reinterpret_cast<HDC>(wParam), *state);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (state) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const bool input = reinterpret_cast<HWND>(lParam) == state->input;
            SetTextColor(dc, input ? palette::text : palette::green);
            SetBkColor(dc, input ? palette::input : palette::background);
            return reinterpret_cast<LRESULT>(input ? state->inputBrush : state->backgroundBrush);
        }
        break;
    case WM_DRAWITEM:
        if (state) {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlID != sendId && item->CtlID != addFileId && item->CtlID != ragToggleId) break;
            if (item->CtlID == ragToggleId) {
                const bool disabled = (item->itemState & ODS_DISABLED) != 0;
                const auto p = [&](int value) { return state->px(value); };
                FillRect(item->hDC, &item->rcItem, state->backgroundBrush);
                RECT track{p(4), p(6), p(48), p(28)};
                COLORREF color = disabled ? palette::border : state->ragEnabled ? palette::green : palette::border;
                rounded(item->hDC, track, color, color, p(22));
                const int left = state->ragEnabled ? p(28) : p(7);
                rounded(item->hDC, {left, p(9), left + p(17), p(26)},
                        state->ragEnabled ? palette::background : palette::muted, color, p(17));
                RECT caption{p(58), 0, item->rcItem.right, item->rcItem.bottom};
                label(item->hDC, state->buttonFont, disabled ? palette::muted : state->ragEnabled ? palette::green : palette::muted,
                      caption, state->ragEnabled ? L"RAG вкл." : L"RAG выкл.");
                if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
                return TRUE;
            }
            bool primary = item->CtlID == sendId;
            bool disabled = (item->itemState & ODS_DISABLED) != 0;
            bool pressed = (item->itemState & ODS_SELECTED) != 0;
            bool hovered = state->hoveredButton == item->hwndItem;
            COLORREF fill = primary ? palette::green : RGB(62, 41, 86);
            if (disabled) fill = primary ? RGB(62, 85, 65) : RGB(42, 32, 55);
            else if (pressed) fill = primary ? RGB(121, 194, 130) : RGB(77, 48, 107);
            else if (hovered) fill = primary ? RGB(179, 249, 185) : RGB(82, 55, 113);
            FillRect(item->hDC, &item->rcItem, primary ? state->inputBrush : state->backgroundBrush);
            rounded(item->hDC, item->rcItem, fill, primary ? fill : palette::border, state->px(14));
            wchar_t text[128];
            GetWindowTextW(item->hwndItem, text, 128);
            label(item->hDC, state->buttonFont, disabled ? palette::muted : primary ? palette::background : palette::text,
                  item->rcItem, text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (item->itemState & ODS_FOCUS) {
                RECT focus = item->rcItem;
                InflateRect(&focus, -state->px(5), -state->px(5));
                DrawFocusRect(item->hDC, &focus);
            }
            return TRUE;
        }
        break;
    case WM_SETFOCUS:
        if (state) SetFocus(state->input);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ragToggleId && HIWORD(wParam) == BN_CLICKED && state) {
            if (!state->busy) {
                state->ragEnabled = !state->ragEnabled;
                SetWindowTextW(state->ragToggle, state->ragEnabled ? L"RAG включён" : L"RAG выключен");
                readyStatus(*state);
                InvalidateRect(state->ragToggle, nullptr, FALSE);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
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
                EnableWindow(state->ragToggle, TRUE);
                readyStatus(*state);
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
    windowClass.hbrBackground = nullptr;
    if (!RegisterClassW(&windowClass)) return 1;
    std::wstring executable(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return 1;
    executable.resize(length);
    WindowState state(std::filesystem::path(executable).parent_path());
    HDC screen = GetDC(nullptr);
    state.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"LocalAgent — Qwen", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, state.px(1040), state.px(820), nullptr, nullptr, instance, &state);
    if (!window) {
        if (state.font) DeleteObject(state.font);
        MessageBoxW(nullptr, L"Не удалось создать окно чата.", L"LocalAgent", MB_ICONERROR);
        return 1;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const COLORREF caption = palette::background, captionText = palette::text;
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36, &captionText, sizeof(captionText));
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
