#include "ao/Overlay.h"

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

namespace ao {

namespace {
constexpr wchar_t kWndClassName[] = L"AndreasOnlineOverlayWndClass";

constexpr int kWndX      = 12;
constexpr int kWndY      = 110;
constexpr int kWndWidth  = 380;
constexpr int kWndHeight = 300;
constexpr int kLineHeight = 16;
constexpr BYTE kWindowAlpha = 195;

constexpr size_t kMaxChatLines = 8;
constexpr size_t kMaxPlayerLines = 10;
constexpr size_t kMaxInputLen = 120; // keeps room under proto::kMaxChatLen (128)

constexpr unsigned int kVkChat   = 'T';
constexpr unsigned int kVkToggle = VK_F10;
} // namespace

struct Overlay::Impl {
    HWND   hwnd = nullptr;
    HANDLE thread = nullptr;
    volatile bool running = false;
    volatile bool stopRequested = false;

    std::mutex mutex;
    std::string status = "AndreasOnline: starting...";
    std::vector<std::pair<uint32_t, std::string>> players;
    std::deque<std::string> chatLines;

    // Typing state: touched only on the overlay thread, so it needs no lock.
    bool typing = false;
    std::string inputBuffer;
    bool prevKeyDown[256] = {};

    std::function<void(const std::string&)> sendCallback;

    void PollKeyboardAndEditBuffer();
    void Paint(HDC hdc);
};

namespace {

bool EdgeDown(Overlay::Impl* impl, unsigned int vk) {
    const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    const bool edge = down && !impl->prevKeyDown[vk & 0xFF];
    impl->prevKeyDown[vk & 0xFF] = down;
    return edge;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        auto* impl = reinterpret_cast<Overlay::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (impl) impl->Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI OverlayThreadProc(LPVOID param) {
    auto* impl = static_cast<Overlay::Impl*>(param);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // IDC_ARROW expands to an ANSI (LPSTR) resource id when UNICODE isn't
    // defined project-wide, which doesn't match LoadCursorW's LPCWSTR param -
    // cast explicitly rather than relying on the project defining UNICODE.
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kWndClassName;
    RegisterClassExW(&wc); // ignore failure: AlreadyExists if we race another instance

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                           WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    HWND hwnd = CreateWindowExW(
        exStyle, kWndClassName, L"AndreasOnline", WS_POPUP,
        kWndX, kWndY, kWndWidth, kWndHeight,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        log::Error("Overlay: CreateWindowExW failed (%lu) - HUD disabled", GetLastError());
        impl->running = false;
        return 0;
    }

    // Set these before ShowWindow: it can trigger an immediate WM_PAINT, and
    // Impl::Paint() (via GWLP_USERDATA) and PollKeyboardAndEditBuffer() both
    // assume impl->hwnd is already valid.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    impl->hwnd = hwnd;
    impl->running = true;

    SetLayeredWindowAttributes(hwnd, 0, kWindowAlpha, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (!impl->stopRequested) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                impl->stopRequested = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        impl->PollKeyboardAndEditBuffer();
        InvalidateRect(hwnd, nullptr, FALSE);

        Sleep(15);
    }

    DestroyWindow(hwnd);
    impl->hwnd = nullptr;
    impl->running = false;
    return 0;
}

} // namespace

void Overlay::Impl::PollKeyboardAndEditBuffer() {
    if (!typing) {
        if (EdgeDown(this, kVkToggle)) {
            // Visibility toggle; handled by the caller via ShowWindow would
            // need extra state - keep it simple: toggle via show/hide here.
            if (hwnd) {
                const bool visible = IsWindowVisible(hwnd) != FALSE;
                ShowWindow(hwnd, visible ? SW_HIDE : SW_SHOWNOACTIVATE);
            }
        }
        if (EdgeDown(this, kVkChat)) {
            typing = true;
            inputBuffer.clear();
        }
        return;
    }

    // While typing, also keep F10 working (chat shouldn't block hiding).
    if (EdgeDown(this, kVkToggle) && hwnd) {
        const bool visible = IsWindowVisible(hwnd) != FALSE;
        ShowWindow(hwnd, visible ? SW_HIDE : SW_SHOWNOACTIVATE);
    }

    if (EdgeDown(this, VK_RETURN)) {
        if (!inputBuffer.empty() && sendCallback) {
            sendCallback(inputBuffer);
        }
        inputBuffer.clear();
        typing = false;
        return;
    }
    if (EdgeDown(this, VK_ESCAPE)) {
        inputBuffer.clear();
        typing = false;
        return;
    }
    if (EdgeDown(this, VK_BACK)) {
        if (!inputBuffer.empty()) inputBuffer.pop_back();
        return;
    }
    if (EdgeDown(this, VK_SPACE)) {
        if (inputBuffer.size() < kMaxInputLen) inputBuffer.push_back(' ');
        return;
    }

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (unsigned int vk = '0'; vk <= '9'; ++vk) {
        if (EdgeDown(this, vk) && inputBuffer.size() < kMaxInputLen) {
            inputBuffer.push_back(static_cast<char>(vk));
        }
    }
    for (unsigned int vk = 'A'; vk <= 'Z'; ++vk) {
        if (EdgeDown(this, vk) && inputBuffer.size() < kMaxInputLen) {
            char c = static_cast<char>(vk);
            if (!shift) c = static_cast<char>(c - 'A' + 'a');
            inputBuffer.push_back(c);
        }
    }
}

void Overlay::Impl::Paint(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    FrameRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    std::string statusCopy;
    std::vector<std::pair<uint32_t, std::string>> playersCopy;
    std::deque<std::string> chatCopy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        statusCopy = status;
        playersCopy = players;
        chatCopy = chatLines;
    }

    int y = 6;
    auto DrawLine = [&](const std::string& text, COLORREF color) {
        SetTextColor(hdc, color);
        TextOutA(hdc, 8, y, text.c_str(), static_cast<int>(text.size()));
        y += kLineHeight;
    };

    DrawLine(statusCopy, RGB(255, 200, 60));
    y += 4;

    char buf[160];
    std::snprintf(buf, sizeof(buf), "Players (%zu):", playersCopy.size());
    DrawLine(buf, RGB(200, 200, 255));
    size_t shown = 0;
    for (const auto& p : playersCopy) {
        if (shown++ >= kMaxPlayerLines) {
            DrawLine("  ...", RGB(150, 150, 150));
            break;
        }
        std::snprintf(buf, sizeof(buf), "  [%u] %s", p.first, p.second.c_str());
        DrawLine(buf, RGB(255, 255, 255));
    }
    y += 4;

    DrawLine("Chat:", RGB(200, 200, 255));
    for (const auto& line : chatCopy) {
        DrawLine(line, RGB(230, 230, 230));
    }
    y += 4;

    if (typing) {
        std::string line = "> " + inputBuffer + "_";
        DrawLine(line, RGB(120, 255, 120));
    } else {
        DrawLine("[T] chat   [F10] hide", RGB(140, 140, 140));
    }
}

Overlay::Overlay() : m_impl(new Impl()) {}

Overlay::~Overlay() {
    Stop();
    delete m_impl;
}

void Overlay::Start() {
    if (m_impl->thread) return; // already started
    m_impl->stopRequested = false;
    m_impl->thread = CreateThread(nullptr, 0, OverlayThreadProc, m_impl, 0, nullptr);
    if (!m_impl->thread) {
        log::Error("Overlay: CreateThread failed (%lu) - HUD disabled", GetLastError());
    }
}

void Overlay::Stop() {
    if (!m_impl->thread) return;
    m_impl->stopRequested = true;
    if (m_impl->hwnd) PostMessageW(m_impl->hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(m_impl->thread, 2000);
    CloseHandle(m_impl->thread);
    m_impl->thread = nullptr;
}

void Overlay::SetStatus(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->status = line;
}

void Overlay::SetPlayers(std::vector<std::pair<uint32_t, std::string>> players) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->players = std::move(players);
}

void Overlay::AddChatLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->chatLines.push_back(line);
    while (m_impl->chatLines.size() > kMaxChatLines) m_impl->chatLines.pop_front();
}

void Overlay::SetSendCallback(std::function<void(const std::string&)> cb) {
    // Only ever written before Start() per the header's contract, and only
    // ever read on the overlay's own thread afterwards - no lock needed.
    m_impl->sendCallback = std::move(cb);
}

} // namespace ao
