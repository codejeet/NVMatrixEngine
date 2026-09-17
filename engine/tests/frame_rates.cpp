#include "../src/frame_rates.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        lab::FrameRates rates;
        require(!rates.sample(10000, 600, 1200) && !rates.ready, "Startup must establish a baseline");
        require(!rates.sample(10250, 615, 1230), "Do not rebuild FPS text every frame");
        require(rates.sample(10500, 630, 1260), "Publish a completed window");
        require(rates.renderFps == 60 && rates.outputFps == 120, "Render/output streams were conflated");
        // Actual presents arrive asynchronously: 0, then a batch. Keep the
        // aggregate instead of inventing one or a configured multiplier per call.
        rates = {};
        rates.sample(0, 0, 0);
        rates.sample(100, 1, 0);
        rates.sample(200, 2, 4);
        rates.sample(300, 3, 4);
        rates.sample(400, 4, 8);
        require(rates.sample(500, 5, 8), "Batched window missing");
        require(rates.renderFps == 10 && rates.outputFps == 16, "Batched counts were clamped or scaled");
        require(rates.sample(1500, 10, 8) && rates.outputFps == 0 && rates.renderFps == 5,
                "No reported presents must remain zero; stalls count in elapsed time");
        require(rates.sample(2100, 40, 38) && rates.outputFps == 50 && rates.renderFps == 50,
                "FG off / no extra frames should show equal throughput");
        require(!rates.sample(2200, 1, 1) && !rates.ready, "Counter restart must rebase without underflow");
        require(!rates.sample(2100, 2, 2) && !rates.ready, "Clock restart must rebase");
        require(!rates.sample(std::numeric_limits<double>::infinity(), 3, 3), "Reject invalid clock");
        require(rates.sample(2600, 2, 2) && rates.outputFps == 0 && rates.renderFps == 0,
                "Idle windows must not retain stale FPS");
        std::cout << "PASS measured render/output FPS: startup, batching, stalls, off, reset, idle\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
