#include "optical_importance.h"
#include "scene.h"
#include <cmath>
#include <cstring>

namespace lab {
OpticalImportance::OpticalImportance(ID3D12Device *device, ID3D12RootSignature *root,
                                     const std::filesystem::path &folder, uint32_t w, uint32_t h, bool check)
    : width(w), height(h), validation(check) {
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        allocatedBytes += bytes;
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    pixels = make(uint64_t(w) * h * 2 * sizeof(Pixel),
                  L"Optics / ping-pong primary geometry and scheduling history");
    counters = make(256, L"Optics / actual rays and selected sample budgets");
    receivers = make(uint64_t(atlasWidth) * chartSize * 16, L"Optics / photon receiver importance");
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Optics / timings and counters");
    if (validation)
        snapshot = gpu::buffer(device, pixels.resource->GetDesc().Width, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                               L"Optics / bounded independent scheduling audit");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 2;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Optical importance timestamps");
    auto code = gpu::bytes(folder / "shaders" / "OpticalFinalize.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root;
    p.CS = {code.data(), code.size()};
    gpu::check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&finalize)), "Optical importance PSO");
}
void OpticalImportance::setWorldBricks(ID3D12Device *device, ID3D12Resource *state, DirectX::XMUINT4 grid,
                                       DirectX::XMFLOAT4 minimumSpacing) {
    const uint32_t capacity = grid.w;
    if (!capacity || capacity > atlasWidth * chartSize)
        throw std::runtime_error("Optical feedback capacity exceeds receiver-clear dispatch");
    if (!feedback.resource || feedback.resource->GetDesc().Width < uint64_t(capacity) * 8) {
        feedback =
            gpu::buffer(device, uint64_t(capacity) * 8, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        L"Optics / interface-brick error feedback");
        allocatedBytes += feedback.resource->GetDesc().Width;
        feedbackReady = false;
        if (validation)
            feedbackSnapshot = gpu::buffer(device, uint64_t(capacity) * 8, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                           L"Optics / independent world-feedback audit");
    }
    world = state;
    worldGrid = grid;
    worldMinimum = minimumSpacing;
}
void OpticalImportance::record(ID3D12GraphicsCommandList *cmd, uint32_t frame) {
    gpu::Event event(cmd, L"Optics / raw variance, edges and caustic importance");
    currentFrame = frame;
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    cmd->SetPipelineState(finalize.Get());
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    gpu::uav(cmd);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
}
void OpticalImportance::recordReadback(ID3D12GraphicsCommandList *cmd) {
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.resource.Get(), 128);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 0, counters.resource.Get(), 0, 64);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (validation) {
        gpu::transition(cmd, pixels.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(snapshot.resource.Get(), pixels.resource.Get());
        gpu::transition(cmd, pixels.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (feedbackSnapshot.resource) {
            gpu::transition(cmd, feedback.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(feedbackSnapshot.resource.Get(), feedback.resource.Get());
            gpu::transition(cmd, feedback.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
}
void OpticalImportance::collect(uint64_t frequency) {
    feedbackReady = world != nullptr;
    void *data;
    D3D12_RANGE range{0, 144}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Optical importance counters");
    std::memcpy(counts.data(), data, sizeof(counts));
    const auto *t = reinterpret_cast<const uint64_t *>(static_cast<const char *>(data) + 128);
    gpuMs = double(t[1] - t[0]) * 1000 / frequency;
    readback.resource->Unmap(0, &written);
    if (currentFrame >= 32) {
        // Keep live play bounded; reports use at most the last 4096 samples.
        if (timings.size() < 4096)
            timings.push_back(gpuMs);
        else
            timings[(currentFrame - 32) % 4096] = gpuMs;
        for (size_t i = 0; i < counts.size(); ++i)
            totals[i] += counts[i];
    }
    if (counts[12])
        throw std::runtime_error("Nonfinite optical scheduling history");
    if (validation)
        audit();
}
void OpticalImportance::audit() {
    void *data;
    D3D12_RANGE range{0, size_t(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Optical independent snapshot");
    const auto *all = static_cast<const Pixel *>(data);
    const uint32_t size = width * height;
    const auto *current = all + (currentFrame & 1) * size;
    const auto *previous = all + ((currentFrame & 1) ^ 1) * size;
    uint32_t hits = 0, samples = 0, invalid = 0, buckets[4]{}, caustics = 0;
    std::vector<DirectX::XMFLOAT2> expectedFeedback(worldGrid.w, {0, 0});
    uint32_t feedbackPixels = 0;
    std::string failure;
    for (uint32_t i = 0; i < size; ++i) {
        const auto &s = current[i];
        for (const auto *v : {&s.positionDepth, &s.normalRoughness, &s.moments, &s.signals})
            for (uint32_t c = 0; c < 4; ++c)
                if (!std::isfinite((&v->x)[c]))
                    failure = "Nonfinite optical pixel";
        const uint32_t n = s.control.w & 255u;
        const bool valid = s.control.z != 0xffffffff;
        float mean = 0, square = 0, age = 1;
        if (valid) {
            if (!currentFrame || s.control.z >= size) {
                failure = "Invalid optical history address";
                break;
            }
            const auto &old = previous[s.control.z];
            if (s.control.x != old.control.x || s.control.y != old.control.y || old.moments.w < 1)
                failure = "Optical reprojection accepted incompatible geometry";
            mean = old.moments.x;
            square = old.moments.y;
            age = std::min(old.moments.w + 1, 32.f);
        }
        const double value = s.moments.z, alpha = 1. / age;
        if (std::abs(s.moments.x - (mean * (1 - alpha) + value * alpha)) > 2e-6 ||
            std::abs(s.moments.y - (square * (1 - alpha) + value * value * alpha)) > 2e-6 ||
            s.moments.w != age)
            failure = "Optical moments do not match independent same-state recurrence";
        if (s.signals.x < 0 || s.signals.x > 1 || s.signals.w < 0 || s.signals.w > 1)
            failure = "Optical importance/confidence outside normalized range";
        const uint32_t address = s.control.w >> 8;
        if (address) {
            if (!world || s.control.y != 8 || address > worldGrid.w) {
                failure = "Optical feedback referenced an incompatible DXR primitive";
                break;
            }
            const float spacing = 8 * worldMinimum.w;
            const float x = (s.positionDepth.x - worldMinimum.x) / spacing,
                        y = (s.positionDepth.y - worldMinimum.y) / spacing,
                        z = (s.positionDepth.z - worldMinimum.z) / spacing;
            const uint32_t id = address - 1;
            const float bx = float(id % worldGrid.x), by = float((id / worldGrid.x) % worldGrid.y),
                        bz = float(id / (worldGrid.x * worldGrid.y));
            // Independently check the point against its actual primitive's closed
            // bounds. A root on a shared face may belong to either adjacent AABB.
            const float epsilon = 1e-5f / spacing;
            if (x < bx - epsilon || x > bx + 1 + epsilon || y < by - epsilon || y > by + 1 + epsilon ||
                z < bz - epsilon || z > bz + 1 + epsilon)
                failure = "Optical feedback root lies outside its DXR brick";
            {
                auto &f = expectedFeedback[id];
                f.x = std::max(f.x, s.signals.x);
                f.y = std::max(f.y, std::max(s.signals.y, s.signals.z));
                ++feedbackPixels;
            }
        }
        if (s.control.x != 0xffffffff) {
            if (!n || n > 8)
                failure = "Invalid optical ray budget";
            ++hits;
            samples += n;
            invalid += !valid;
            ++buckets[n <= 1 ? 0 : (n <= 2 ? 1 : (n <= 4 ? 2 : 3))];
            caustics += s.signals.z > .05f;
        } else if (n)
            failure = "Miss allocated diffuse ray samples";
    }
    snapshot.resource->Unmap(0, &written);
    if (feedbackSnapshot.resource) {
        range = {0, size_t(worldGrid.w) * 8};
        gpu::check(feedbackSnapshot.resource->Map(0, &range, &data), "Optical feedback snapshot");
        const auto *actual = static_cast<const DirectX::XMFLOAT2 *>(data);
        if (feedbackPixels != counts[14])
            failure = "Optical feedback pixel count at frame " + std::to_string(currentFrame) + ": CPU " +
                      std::to_string(feedbackPixels) + " GPU " + std::to_string(counts[14]);
        for (uint32_t i = 0; i < worldGrid.w; ++i)
            if (std::memcmp(actual + i, expectedFeedback.data() + i, sizeof(*actual))) {
                failure = "Optical feedback at frame " + std::to_string(currentFrame) + " brick " +
                          std::to_string(i) + ": CPU (" + std::to_string(expectedFeedback[i].x) + "," +
                          std::to_string(expectedFeedback[i].y) + ") GPU (" + std::to_string(actual[i].x) +
                          "," + std::to_string(actual[i].y) + ")";
                break;
            }
        feedbackSnapshot.resource->Unmap(0, &written);
        ++feedbackAudits;
    }
    if (hits != counts[3] || samples != counts[5] || invalid != counts[6] || caustics != counts[11])
        failure = "Optical pixel/work counter mismatch";
    for (uint32_t i = 0; i < 4; ++i)
        if (buckets[i] != counts[7 + i])
            failure = "Optical budget histogram mismatch at frame " + std::to_string(currentFrame) +
                      " bucket " + std::to_string(i) + ": CPU " + std::to_string(buckets[i]) + " GPU " +
                      std::to_string(counts[7 + i]) + "; hits " + std::to_string(hits) + "/" +
                      std::to_string(counts[3]);
    if (!failure.empty())
        throw std::runtime_error(failure);
    ++audits;
}
void OpticalImportance::report(std::ostream &out) const {
    auto sorted = timings;
    std::sort(sorted.begin(), sorted.end());
    out << "{\"allocatedBytes\":" << allocatedBytes << ",\"audits\":" << audits
        << ",\"feedbackAudits\":" << feedbackAudits
        << ",\"classificationMedianMs\":" << (sorted.empty() ? 0 : sorted[sorted.size() / 2])
        << ",\"sampleCount\":" << sorted.size() << ",\"last\":[";
    for (size_t i = 0; i < counts.size(); ++i)
        out << (i ? "," : "") << counts[i];
    out << "],\"totals\":[";
    for (size_t i = 0; i < totals.size(); ++i)
        out << (i ? "," : "") << totals[i];
    out << "],\"columns\":[\"cameraRays\",\"temporalRays\",\"spatialRays\",\"hitPixels\","
           "\"initialPaths\",\"selectedSamples\",\"rejectedHistoryPixels\",\"oneSamplePixels\","
           "\"twoSamplePixels\",\"threeOrFourSamplePixels\",\"fiveToEightSamplePixels\","
           "\"causticPixels\",\"invalid\",\"maxHistoryAge\",\"feedbackPixels\",\"physicsPriorPixels\"]}";
}
} // namespace lab
