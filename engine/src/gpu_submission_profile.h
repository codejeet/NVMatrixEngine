#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <stdexcept>

namespace lab::gpu {
// CPU wall-clock observations, not GPU execution timestamps. In particular,
// the resumed DX12 list is submitted only after RR/UI recording: the GPU's
// CUDA ownership span can include that CPU delay. No fences or GPU queries here.
// Construct only for bounded --profile-latency runs; ordinary play passes null.
class SubmissionTimeline {
  public:
    enum class Stage {
        RenderBegin,
        FluidBegin,
        CudaPrepareBegin,
        CudaPrepared,
        InteropBegin,
        InteropContextReady,
        PrefixClosed,
        PrefixSubmitted,
        DxReleased,
        CudaWaitQueued,
        CudaStartQueued,
        CudaWorkQueued,
        CudaEndQueued,
        CudaReleaseQueued,
        DxWaitQueued,
        InteropResumed,
        FluidRecorded,
        SurfaceRecorded,
        PhotonsRecorded,
        CameraRecorded,
        CompositeRecorded,
        RRBegin,
        RRRecorded,
        UIRecorded,
        FrameClosed,
        FrameSubmitted,
        Presented,
        FrameFenceSignaled,
        FrameEventQueued,
        FrameComplete,
        Collected,
        Count
    };
    using Clock = std::chrono::steady_clock;
    static constexpr size_t count = size_t(Stage::Count);
    static constexpr std::array<const char *, count> columns{"renderBegin",       "fluidBegin",
                                                             "cudaPrepareBegin",  "cudaPrepared",
                                                             "interopBegin",      "interopContextReady",
                                                             "prefixClosed",      "prefixSubmitted",
                                                             "dxReleased",        "cudaWaitQueued",
                                                             "cudaStartQueued",   "cudaWorkQueued",
                                                             "cudaEndQueued",     "cudaReleaseQueued",
                                                             "dxWaitQueued",      "interopResumed",
                                                             "fluidRecorded",     "surfaceRecorded",
                                                             "photonsRecorded",   "cameraRecorded",
                                                             "compositeRecorded", "rrBegin",
                                                             "rrRecorded",        "uiRecorded",
                                                             "frameClosed",       "frameSubmitted",
                                                             "presented",         "frameFenceSignaled",
                                                             "frameEventQueued",  "frameComplete",
                                                             "collected"};
    explicit SubmissionTimeline(Clock::time_point start = Clock::now()) : start(start) {
        offsets.fill(-1);
        offsets[0] = 0;
    }
    void mark(Stage stage) {
        const size_t index = size_t(stage);
        if (index >= count || index <= last)
            throw std::runtime_error("Duplicate or out-of-order CPU submission timestamp");
        offsets[index] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        last = index;
    }
    double at(Stage stage) const {
        return offsets.at(size_t(stage));
    }
    void writeRow(std::ostream &out, uint32_t frame) const {
        const auto precision = out.precision();
        out << std::setprecision(9) << '[' << frame;
        for (const double offset : offsets) {
            out << ',';
            if (offset < 0)
                out << "null"; // An unexecuted stage is not an instantaneous operation.
            else
                out << offset;
        }
        out << ']';
        out.precision(precision);
    }

  private:
    Clock::time_point start;
    std::array<double, count> offsets;
    size_t last = 0;
};
using SubmissionStage = SubmissionTimeline::Stage;
inline void stamp(SubmissionTimeline *timeline, SubmissionStage stage) {
    if (timeline)
        timeline->mark(stage);
}
} // namespace lab::gpu
