#pragma once
#include "gpu_resources.h"
#include "gpu_submission_profile.h"
#include <dxgi1_4.h>
#include <array>
#include <chrono>
#include <ostream>
#include <vector>

namespace lab {
// Bounded diagnostic recording, not an adaptive quality controller. No GPU
// state readback or extra fence: timings come from the existing frame fence.
class LatencyProfile {
  public:
    LatencyProfile(IDXGIAdapter1 *adapter, uint32_t capacity) : capacity(capacity) {
        if (!capacity || capacity > 36000)
            throw std::runtime_error("Latency profiling requires bounded --frames=1..36000");
        gpu::check(adapter->QueryInterface(IID_PPV_ARGS(&memoryAdapter)), "Memory budget adapter");
        records.reserve(capacity);
        submissions.reserve(capacity);
    }
    void rendered(uint32_t frame, const std::array<double, 11> &timing, const std::array<double, 5> &cuda,
                  double advanced, double dropped, const gpu::SubmissionTimeline &submission) {
        if (pending || records.size() == capacity || frame != records.size())
            throw std::runtime_error("Incomplete or overflowing latency frame");
        Row row{};
        std::copy(timing.begin(), timing.end(), row.begin() + 2);
        std::copy(cuda.begin(), cuda.end(), row.begin() + 13);
        row[0] = frame;
        simulatedSeconds += advanced;
        row[18] = simulatedSeconds;
        row[19] = dropped;
        records.push_back(row);
        submissions.push_back(submission);
        pending = true;
    }
    void finish(double wholeFrameMs, double applicationElapsedMs) {
        if (!pending)
            throw std::runtime_error("Missing rendered latency frame");
        const auto start = std::chrono::steady_clock::now();
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        gpu::check(memoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info),
                   "Profile process local-memory usage");
        const double queryMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        auto &row = records.back();
        row[1] = wholeFrameMs + queryMs;
        row[20] = double(info.CurrentUsage);
        row[21] = double(info.Budget);
        row[22] = queryMs;
        peakLocalUsage = std::max(peakLocalUsage, info.CurrentUsage);
        if (records.size() == 1) {
            startupMs = applicationElapsedMs - wholeFrameMs;
            firstFrameCompletedMs = applicationElapsedMs + queryMs;
        }
        pending = false;
    }
    void report(std::ostream &o) const {
        o << "{\"version\":1,\"startupMs\":" << startupMs
          << ",\"firstFrameCompletedMs\":" << firstFrameCompletedMs
          << ",\"sampledPeakLocalUsageBytes\":" << peakLocalUsage
          << ",\"memorySource\":\"DXGI per-process local segment; sampled once per completed frame\""
          << ",\"completeFrames\":" << records.size() - (pending ? 1 : 0)
          << ",\"columns\":[\"frameIndex\",\"wholeFrame\",\"photons\",\"atlasEma\",\"camera\","
             "\"composite\",\"dlssRR\",\"rendererFrame\",\"fluidSimulation\",\"fluidReconstruction\","
             "\"fluidBlas\",\"whitewaterSimulation\",\"whitewaterBlas\",\"cudaWork\",\"cudaHandoff\","
             "\"cudaSpan\",\"cudaCpuEnqueue\",\"cudaPreparationTotal\",\"simulatedSeconds\","
             "\"droppedSeconds\",\"localUsageBytes\",\"localBudgetBytes\",\"memoryQueryMs\"],\"rows\":[";
        for (size_t i = 0; i < records.size() - (pending ? 1 : 0); ++i) {
            o << (i ? ",[" : "[");
            for (size_t j = 0; j < records[i].size(); ++j)
                o << (j ? "," : "") << records[i][j];
            o << ']';
        }
        o << "],\"cpuSubmission\":{\"version\":1,\"clock\":\"steady_clock\","
             "\"units\":\"milliseconds since renderBegin; CPU wall time, not GPU time\","
             "\"columns\":[\"frameIndex\"";
        for (const auto *name : gpu::SubmissionTimeline::columns)
            o << ",\"" << name << '"';
        o << "],\"rows\":[";
        for (size_t i = 0; i < records.size() - (pending ? 1 : 0); ++i) {
            if (i)
                o << ',';
            submissions[i].writeRow(o, uint32_t(i));
        }
        o << "]}}";
    }

  private:
    using Row = std::array<double, 23>;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> memoryAdapter;
    std::vector<Row> records;
    std::vector<gpu::SubmissionTimeline> submissions;
    uint32_t capacity;
    uint64_t peakLocalUsage = 0;
    double startupMs = 0, firstFrameCompletedMs = 0, simulatedSeconds = 0;
    bool pending = false;
};
} // namespace lab
