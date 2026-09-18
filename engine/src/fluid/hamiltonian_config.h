#pragma once
#include <cmath>
#include <stdexcept>

namespace lab {
// Wang et al., Hamiltonian Two-Way Coupling of Nonlinear Waves and 3D
// Flows (2026), https://arxiv.org/abs/2608.25203. SI units throughout.
struct HamiltonianConfig {
    bool enabled = false;
    bool adaptive = true; // disconnected 3D regions selected by wet solids and fluid activity
    bool extendOpticalSurface = false; // Continue the ocean into its distant optical horizon.
    unsigned order = 2; // The authors' coupled implementation defaults to HOS-2.
    unsigned resolution = 32;
    float windSpeed = 0; // Nonzero: energy-normalized wind spectrum; amplitude is ~half significant wave height.
    float epsilon = .2f, amplitude = .035f, relaxation = 10.f;
    void validate(float depth, float gravity) const {
        if ((resolution != 32 && resolution != 64 && resolution != 128) || !std::isfinite(windSpeed) || windSpeed < 0 || windSpeed > 30 ||
            (order != 2 && order != 3) || !std::isfinite(epsilon) || epsilon < 0 || epsilon > 1 ||
            !std::isfinite(amplitude) || amplitude < 0 || amplitude > .15f * depth ||
            !std::isfinite(relaxation) || relaxation < 0 || relaxation > 100 || !std::isfinite(depth) ||
            depth <= 0 || !std::isfinite(gravity) || gravity <= 0)
            throw std::runtime_error("Hamiltonian waves need order 2 or 3, epsilon [0,1], amplitude "
                                     "[0,0.15*depth] metres, relaxation [0,100]/s and positive gravity");
    }
};
} // namespace lab
