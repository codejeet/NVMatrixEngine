#pragma once
#include <cmath>
#include <cstdint>

namespace lab {
// Measure both streams over the same wall-clock window. DLSS may report zero
// presents in one query and a batch in the next; never clamp or multiply them.
struct FrameRates {
    double renderFps = 0, outputFps = 0;
    bool ready = false;
    bool sample(double nowMs, uint64_t rendered, uint64_t presented) {
        if (!std::isfinite(nowMs))
            return false;
        if (!started || nowMs < startMs || rendered < startRendered || presented < startPresented) {
            startMs = nowMs;
            startRendered = rendered;
            startPresented = presented;
            started = true;
            ready = false;
            return false;
        }
        const double elapsed = nowMs - startMs;
        if (elapsed < 500)
            return false;
        renderFps = 1000.0 * double(rendered - startRendered) / elapsed;
        outputFps = 1000.0 * double(presented - startPresented) / elapsed;
        startMs = nowMs;
        startRendered = rendered;
        startPresented = presented;
        ready = true;
        return true;
    }

  private:
    bool started = false;
    double startMs = 0;
    uint64_t startRendered = 0, startPresented = 0;
};
} // namespace lab
