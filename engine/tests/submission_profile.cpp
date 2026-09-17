#include "../src/gpu_submission_profile.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

using lab::gpu::SubmissionTimeline;
using Stage = SubmissionTimeline::Stage;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F &&f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "Invalid CPU timeline ordering accepted");
}
int main() try {
    SubmissionTimeline timeline;
    require(timeline.at(Stage::RenderBegin) == 0, "CPU origin must be zero");
    require(timeline.at(Stage::CudaPrepared) < 0, "Unexecuted CPU stage must be missing");
    lab::gpu::stamp(nullptr, Stage::Count); // Disabled profiling never reads the clock or stage.
    rejects([&] { timeline.mark(Stage::RenderBegin); });
    timeline.mark(Stage::FluidBegin);
    timeline.mark(Stage::FluidRecorded); // DX12 has no CUDA marks.
    rejects([&] { timeline.mark(Stage::CudaPrepared); });
    rejects([&] { timeline.mark(Stage::FluidRecorded); });
    rejects([&] { timeline.mark(Stage::Count); });
    timeline.mark(Stage::Collected);
    require(timeline.at(Stage::Collected) >= timeline.at(Stage::FluidRecorded), "Nonmonotonic CPU clock");
    auto copy = timeline;
    std::ostringstream out;
    out.precision(4);
    copy.writeRow(out, 17);
    const auto row = out.str();
    require(out.precision() == 4, "Timeline serialization leaked output precision");
    require(row.starts_with("[17,0,") && row.back() == ']', "Missing frame identity/origin");
    require(std::count(row.begin(), row.end(), ',') == SubmissionTimeline::count, "Wrong CPU row width");
    require(row.find("null,null") != std::string::npos, "Missing timestamps serialized as zero");
    SubmissionTimeline complete;
    double previous = 0;
    for (size_t i = 1; i < SubmissionTimeline::count; ++i) {
        complete.mark(Stage(i));
        require(complete.at(Stage(i)) >= previous, "CPU timestamp ordering");
        previous = complete.at(Stage(i));
        require(SubmissionTimeline::columns[i][0] != '\0', "Missing CPU schema column");
    }
    std::cout << "PASS CPU submission timeline: monotonic ordering, null holes, copy, serialization, "
                 "disabled path\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL CPU submission timeline: " << e.what() << '\n';
    return 1;
}
