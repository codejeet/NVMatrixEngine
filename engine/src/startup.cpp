#include "startup.h"
#include <algorithm>
#include <stdexcept>

namespace lab {
StartupScreen::StartupScreen(HWND hwnd, std::wstring name) : window(hwnd), scene(std::move(name)) {
    // For an HWND timer, WM_TIMER carries the requested ID, not necessarily
    // SetTimer's success return value (in particular when the requested ID is 0).
    constexpr UINT_PTR loadingTimer = 0x4e564d;
    if (!SetTimer(window, loadingTimer, 100, nullptr))
        throw std::runtime_error("Could not start loading screen timer");
    timer = loadingTimer;
    SetWindowTextW(window, (scene + L" | Loading - " + status).c_str());
}
StartupScreen::~StartupScreen() { finish(); }
void StartupScreen::finish() {
    if (timer) KillTimer(window, timer);
    timer = 0;
    finished = true;
}
void StartupScreen::phase(const wchar_t *text) {
    pump();
    if (canceled) throw StartupCancelled{};
    status = text;
    SetWindowTextW(window, (scene + L" | Loading - " + status).c_str());
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
}
void StartupScreen::cancel() {
    if (canceled) return;
    canceled = true;
    status = L"Closing - waiting for the current setup step";
    SetWindowTextW(window, (scene + L" | Closing").c_str());
    InvalidateRect(window, nullptr, FALSE);
}
void StartupScreen::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { cancel(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
bool StartupScreen::message(UINT msg, WPARAM w, LPARAM, LRESULT &result) {
    if (finished) return false;
    result = 0;
    switch (msg) {
    case WM_PAINT: paint(); return true;
    case WM_ERASEBKGND: result = 1; return true;
    case WM_TIMER:
        if (w != timer) return false;
        InvalidateRect(window, nullptr, FALSE);
        return true;
    case WM_CLOSE: cancel(); return true;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) cancel();
        return true;
    case WM_KEYUP: case WM_CHAR:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: return true;
    case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return false;
    default: return false;
    }
}
void StartupScreen::paint() {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(window, &ps);
    RECT client{}; GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    if (width > 0 && height > 0) {
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        auto oldBitmap = SelectObject(dc, bitmap);
        auto fill = [&](RECT rect, COLORREF color) {
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        };
        fill(client, RGB(8, 17, 25));
        const float scale = float(GetDpiForWindow(window)) / 96.f;
        const int margin = std::min(width / 10, int(64 * scale));
        const int left = std::max(margin, (width - int(600 * scale)) / 2);
        const int right = std::min(width - margin, left + int(600 * scale));
        const int top = std::max(int(24 * scale), (height - int(270 * scale)) / 2);
        SetBkMode(dc, TRANSPARENT);
        auto text = [&](const std::wstring &value, int y, int size, COLORREF color, int weight = FW_NORMAL) {
            HFONT font = CreateFontW(-int(size * scale), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Segoe UI");
            auto old = SelectObject(dc, font);
            SetTextColor(dc, color);
            RECT rect{left, top + int(y * scale), right, top + int((y + size * 3) * scale)};
            DrawTextW(dc, value.c_str(), -1, &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, old);
            DeleteObject(font);
        };
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        text(L"N V M A T R I X E N G I N E", 0, 13, RGB(108, 228, 221));
        text(canceled ? L"Closing scene" : L"Loading " + scene, 30, 34, RGB(229, 240, 247), FW_SEMIBOLD);
        text(status, 95, 18, RGB(207, 226, 236));
        RECT bar{left, top + int(143 * scale), right, top + int(148 * scale)};
        fill(bar, RGB(32, 55, 69));
        // Indeterminate activity, not a made-up completion percentage.
        const int segment = std::max(1, (right - left) / 5);
        const auto tick = float(milliseconds % 2400) / 1200.f;
        const float fraction = tick < 1 ? tick : 2 - tick;
        bar.left += int((right - left - segment) * fraction);
        bar.right = bar.left + segment;
        fill(bar, RGB(108, 228, 221));
        text(L"Elapsed: " + std::to_wstring(milliseconds / 1000) + L" seconds", 167, 14, RGB(153, 178, 194));
        text(L"The first launch can take a few minutes. Later launches are faster.", 200, 15, RGB(153, 178, 194));
        text(canceled ? L"The window will close when this step finishes." : L"Esc to cancel", 248, 13, RGB(153, 178, 194));
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    EndPaint(window, &ps);
}
}
