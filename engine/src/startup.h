#pragma once
#include <windows.h>
#include <chrono>
#include <exception>
#include <future>
#include <string>
#include <utility>

namespace lab {
struct StartupCancelled : std::exception {
    const char *what() const noexcept override { return "Startup canceled"; }
};

// Owned by the window thread. Jobs never access this object or the window UI;
// get() joins each job before any renderer state becomes available to gameplay.
class StartupScreen {
  public:
    StartupScreen(HWND window, std::wstring scene);
    ~StartupScreen();
    StartupScreen(const StartupScreen &) = delete;
    StartupScreen &operator=(const StartupScreen &) = delete;
    void phase(const wchar_t *text);
    void finish();
    bool message(UINT msg, WPARAM w, LPARAM l, LRESULT &result);
    template<class Work> auto run(const wchar_t *text, Work &&work) {
        phase(text);
        auto job = std::async(std::launch::async, std::forward<Work>(work));
        while (job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            pump();
            // Wake immediately for input, and periodically for job completion.
            MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        pump();
        if (canceled) {
            // A driver compile cannot be interrupted. Keep pumping until it
            // completes, then join before releasing any captured resources.
            job.wait();
            throw StartupCancelled{};
        }
        return job.get(); // Preserve worker exceptions on the window thread.
    }

  private:
    void pump();
    void cancel();
    void paint();
    HWND window;
    UINT_PTR timer = 0;
    std::wstring scene, status = L"Starting graphics";
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool canceled = false, finished = false;
};
}
