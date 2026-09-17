#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lab {
struct FluidDouble2 {
    double x, y;
};
struct FluidDouble4 {
    double x, y, z, w;
};
static_assert(sizeof(FluidDouble2) == 16 && sizeof(FluidDouble4) == 32);

// Independent CPU audits must decode the actual resource format, including
// unaligned byte-packed snapshots. Never downcast FP64 authoritative storage
// to the float UI/cache representation before checking conservation.
template <class Value> std::vector<Value> readFluidNumbers(const void *data, uint32_t count, bool precise) {
    std::vector<Value> result(count);
    const auto bytes = static_cast<const char *>(data);
    constexpr size_t components = sizeof(Value) / sizeof(double);
    for (uint32_t i = 0; i < count; ++i) {
        double values[components]{};
        for (size_t a = 0; a < components; ++a) {
            const size_t k = size_t(i) * components + a;
            if (precise)
                memcpy(&values[a], bytes + k * 8, 8);
            else {
                float v;
                memcpy(&v, bytes + k * 4, 4);
                values[a] = v;
            }
        }
        memcpy(&result[i], values, sizeof(Value));
    }
    return result;
}
} // namespace lab
