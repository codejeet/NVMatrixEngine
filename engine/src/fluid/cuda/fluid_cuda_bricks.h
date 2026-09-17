#pragma once
#include <cstddef>
#include <cstdint>

namespace lab::cuda_fluid {
// Fixed-domain page table, sparse physical 4^3 bricks. Fields are double buffered
// so an incomplete allocation/field build cannot overwrite the published state.
struct BrickConfig {
    uint32_t nx = 0, ny = 0, nz = 0, capacity = 0, changesPerFrame = 64;
    uint32_t bytesPerCell = 0;
};
enum BrickCounter : uint32_t {
    BrickFront,
    BrickCandidate,
    BrickReady,
    BrickVersion,
    BrickRemaining,
    BrickRequired,
    BrickMissing,
    BrickResident,
    BrickBacklogPeak,
    BrickResidentPeak,
    BrickAllocations,
    BrickRetirements,
    BrickCommits,
    BrickDeferrals,
    BrickOverflows,
    BrickInvalid,
    BrickFrameChanges,
    BrickPeakFrameChanges,
    BrickCounterCount
};
struct BrickView {
    uint32_t bx, by, bz, bricks, capacity, bytesPerCell;
    uint32_t *pages[2], *keys[2], *active[2], *requests, *control;
    void *fields[2];
};
struct BrickPool;
BrickPool *createBricks(const BrickConfig &);
void destroyBricks(BrickPool *) noexcept;
BrickView brickView(const BrickPool *);
size_t brickBytes(const BrickPool *);
// Stream-ordered operations, all capture-safe and allocation-free. Requests are
// an engine-generated 0/1 flag per virtual brick; no CPU topology readbacks.
void beginBrickFrame(BrickPool *, void *stream);
void clearBrickRequests(BrickPool *, void *stream);
void stageBricks(BrickPool *, void *stream);
// Caller must finish candidate field/ownership initialization before publishing.
// CandidateReady==0 (or Invalid!=0) preserves the complete previous front state.
void publishBricks(BrickPool *, void *stream);
} // namespace lab::cuda_fluid
