#include "../src/startup.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
lab::StartupScreen *screen = nullptr;
unsigned paints = 0, heartbeats = 0;
LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_APP + 1) { ++heartbeats; return 1; }
    if (msg == WM_PAINT) ++paints;
    LRESULT result = 0;
    if (screen && screen->message(msg, w, l, result)) return result;
    return DefWindowProcW(window, msg, w, l);
}
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
}
int main() {
    HWND window = nullptr;
    try {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = proc; wc.lpszClassName = L"NVMatrixStartupTest";
        require(RegisterClassW(&wc) != 0, "Register startup test window");
        window = CreateWindowW(wc.lpszClassName, L"Startup test", WS_OVERLAPPEDWINDOW,
            60, 60, 850, 500, nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "Create startup test window");
        {
            lab::StartupScreen loading(window, L"Startup check"); screen = &loading;
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
            const auto uiThread = std::this_thread::get_id();
            std::atomic<unsigned> missed{0};
            std::jthread observer([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    DWORD_PTR result = 0;
                    auto response = SendMessageTimeoutW(window, WM_APP + 1, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result);
                    if (!stop.stop_requested() && (!response || result != 1)) ++missed;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            int answer = loading.run(L"Checking window responsiveness", [&] {
                require(std::this_thread::get_id() != uiThread, "Startup job ran on the window thread");
                // Longer than Windows' hung-window threshold, independent of
                // whether a particular GPU already has a warm shader cache.
                std::this_thread::sleep_for(std::chrono::milliseconds(6500));
                return 42;
            });
            observer.request_stop(); observer.join();
            require(answer == 42, "Worker result was lost");
            require(missed == 0 && heartbeats > 20, "Window stopped responding during startup");
            require(paints > 20, "Loading animation did not repaint during background work");
            bool caught = false;
            try { loading.run(L"Checking errors", [] { throw std::runtime_error("worker failure"); }); }
            catch (const std::runtime_error &e) { caught = std::string(e.what()) == "worker failure"; }
            require(caught, "Worker exception did not reach the window thread");
            loading.finish(); screen = nullptr;
        }
        // Closing during a job joins the worker before any captured data can be
        // destroyed, and prevents subsequent initialization phases from starting.
        {
            lab::StartupScreen loading(window, L"Cancellation check"); screen = &loading;
            std::atomic<bool> joined{false};
            std::jthread closer([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                PostMessageW(window, WM_CLOSE, 0, 0);
            });
            bool canceled = false, nextRan = false;
            try { loading.run(L"Checking cancellation", [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300)); joined = true;
            }); } catch (const lab::StartupCancelled &) { canceled = true; }
            require(canceled && joined, "Close did not safely cancel startup");
            try { loading.run(L"Must not run", [&] { nextRan = true; }); }
            catch (const lab::StartupCancelled &) {}
            require(!nextRan, "A new startup job ran after cancellation");
            loading.finish(); screen = nullptr;
        }
        DestroyWindow(window);
        std::cout << "PASS: startup worker, live painting, window responsiveness, results, errors and cancellation\n";
        return 0;
    } catch (const std::exception &e) {
        screen = nullptr;
        if (window) DestroyWindow(window);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
