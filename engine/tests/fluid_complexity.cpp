#include <cmath>
#include <cstdlib>
#include <iostream>
using uint = unsigned int;
#include "../shaders/fluid/complexity-policy.hlsli"
static void require(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    auto lod = [](uint old, float value) {
        return complexityLod(old, value, .75f, .45f, .15f, .55f, .30f, .08f);
    };
    require(lod(3, 1) == 0, "Topology/body trigger must promote immediately");
    require(lod(0, 0) == 3, "Calm interior can request coarse representation");
    require(lod(0, .65f) == 0 && lod(1, .65f) == 1, "Fine hysteresis gap");
    require(lod(1, .37f) == 1 && lod(2, .37f) == 2, "Medium hysteresis gap");
    require(lod(2, .11f) == 2 && lod(3, .11f) == 3, "Coarse hysteresis gap");
    require(lod(1, .75f) == 1 && lod(0, .55f) == 0, "Threshold equality must not oscillate");
    for (uint old = 0; old < 4; ++old) {
        uint previous = 3;
        for (uint i = 0; i <= 1000; ++i) {
            uint next = lod(old, i / 1000.f);
            require(next <= 3 && next <= previous, "Importance must monotonically refine requests");
            require(lod(next, i / 1000.f) == next, "Policy must reach a stable fixed point");
            previous = next;
        }
    }
    // Exact padded brick / MAC mapping: non-cubic partial edge bricks cover every cell once.
    for (uint nx : {1u, 4u, 5u, 75u})
        for (uint ny : {1u, 19u})
            for (uint nz : {7u, 88u}) {
                uint bx = (nx + 7) / 4, by = (ny + 7) / 4, bz = (nz + 7) / 4, cells = 0;
                for (uint z = 0; z < bz; ++z)
                    for (uint y = 0; y < by; ++y)
                        for (uint x = 0; x < bx; ++x)
                            for (uint lane = 0; lane < 64; ++lane) {
                                int cx = int(4 * x + lane % 4) - 2, cy = int(4 * y + (lane / 4) % 4) - 2,
                                    cz = int(4 * z + lane / 16) - 2;
                                cells += cx >= 0 && cy >= 0 && cz >= 0 && uint(cx) < nx && uint(cy) < ny &&
                                         uint(cz) < nz;
                            }
                require(cells == nx * ny * nz, "Brick/MAC coverage must preserve particle accounting");
            }
    std::cout << "PASS complexity policy: hysteresis, immediate promotion, monotonicity, fixed points, "
                 "padded MAC coverage\n";
}
