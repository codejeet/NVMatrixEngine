#pragma once
#include <cuda_runtime.h>
#include <cmath>

namespace lab::cuda_fluid::geometry {
// Original analytic integration of a half-space over a Cartesian box. Sorted
// piecewise polynomials avoid subtracting nearly equal cubes for small normals.
__host__ __device__ inline double fraction(double3 n, double alpha) {
    double a = fabs(n.x), b = fabs(n.y), c = fabs(n.z);
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        double t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    // alpha is measured from the liquid-side corner in reflected coordinates.
    // Do not add/subtract a unit-cell offset: that would erase tiny intervals
    // near x=1 for a negative normal through cancellation.
    const double sum = a + b + c;
    if (!sum)
        return fmin(1., fmax(0., alpha)); // Unresolved uniform mixture, not a fictitious plane.
    if (alpha <= 0)
        return 0;
    if (alpha >= sum)
        return 1;
    const bool complement = alpha > sum * .5;
    if (complement)
        alpha = sum - alpha;
    double value;
    if (!b)
        value = alpha / c;
    else if (!a)
        value = alpha < b ? (alpha / b) * (alpha / c) * .5 : (alpha - b * .5) / c;
    else if (alpha < a)
        value = (alpha / a) * (alpha / b) * (alpha / c) / 6;
    else if (alpha >= a + b)
        value = (alpha - (a + b) * .5) / c;
    else {
        // Normalize before multiplying. Squared lengths can underflow even
        // when their final dimensionless volume fraction is a normal double.
        double v = 3 * (alpha / b) * ((alpha - a) / c) + (a / b) * (a / c);
        if (alpha > b) {
            const double d = alpha - b;
            v -= (d / a) * (d / b) * (d / c);
        }
        if (alpha > c) {
            const double d = alpha - c;
            v -= (d / a) * (d / b) * (d / c);
        }
        value = v / 6;
    }
    return fmin(1., fmax(0., complement ? 1 - value : value));
}
__host__ __device__ inline double intercept(double3 n, double f) {
    double a = fabs(n.x), b = fabs(n.y), c = fabs(n.z);
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        double t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    const double sum = a + b + c;
    if (!sum)
        return f;
    if (f <= 0)
        return 0;
    if (f >= 1)
        return sum;
    const bool complement = f > .5;
    const double target = complement ? 1 - f : f;
    double alpha;
    if (!b)
        alpha = c * target;
    else if (!a) {
        const double product = 2 * b * c * target;
        alpha =
            target < b / (2 * c)
                ? (product >= 2.2250738585072014e-308 ? sqrt(product) : sqrt(2 * b) * sqrt(c) * sqrt(target))
                : c * target + b * .5;
    } else if (target < (a / b) * (a / c) / 6) {
        const double product = 6 * a * b * c * target;
        alpha = product >= 2.2250738585072014e-308 ? cbrt(product)
                                                   : cbrt(6 * target) * cbrt(a) * cbrt(b) * cbrt(c);
    } else if (target <= (3 * (b - a) + a * (a / b)) / (6 * c)) {
        const double product = 2 * b * c * target;
        alpha = a * .5 + (product >= 2.2250738585072014e-308
                              ? sqrt(product - a * a / 12)
                              : sqrt(b) * sqrt(c) * sqrt(2 * target - (a / b) * (a / c) / 12));
    } else if (a + b <= c && target >= (a + b) / (2 * c))
        alpha = c * target + (a + b) * .5;
    else {
        // The corner, quadratic and linear branches above resolve small
        // intercepts analytically. The remaining interval is [b, min(a+b,
        // sum/2)], with hi <= 2*lo. A fixed unit-cell bracket loses relative
        // precision for thin liquid regions and nearly axis-aligned planes.
        double lo = b, hi = fmin(a + b, sum * .5);
        const double3 positive = make_double3(a, b, c);
        for (uint32_t i = 0; i < 52; ++i) {
            const double middle = (lo + hi) * .5;
            if (fraction(positive, middle) < target)
                lo = middle;
            else
                hi = middle;
        }
        alpha = (lo + hi) * .5;
    }
    return complement ? sum - alpha : alpha;
}
__host__ __device__ inline double boxFraction(double4 plane, double3 lo, double3 hi) {
    const double3 n = make_double3(plane.x, plane.y, plane.z);
    if (n.x == 0 && n.y == 0 && n.z == 0)
        return plane.w;
    const double3 a = make_double3(fabs(n.x), fabs(n.y), fabs(n.z));
    return fraction(make_double3(a.x * (hi.x - lo.x), a.y * (hi.y - lo.y), a.z * (hi.z - lo.z)),
                    plane.w - a.x * (n.x >= 0 ? lo.x : 1 - hi.x) - a.y * (n.y >= 0 ? lo.y : 1 - hi.y) -
                        a.z * (n.z >= 0 ? lo.z : 1 - hi.z));
}
__host__ __device__ inline double slabFraction(double4 plane, uint32_t axis, double width, bool upper) {
    double3 n = make_double3(fabs(plane.x), fabs(plane.y), fabs(plane.z));
    if (n.x == 0 && n.y == 0 && n.z == 0)
        return plane.w;
    const double component = (&n.x)[axis];
    const bool farCorner = upper == ((&plane.x)[axis] >= 0);
    // This form also resolves a swept width much smaller than one ulp at 1.
    const double alpha = farCorner ? (plane.w - component) + component * width : plane.w;
    (&n.x)[axis] *= width;
    return fraction(n, alpha);
}
__host__ __device__ inline bool slabContains(double4 plane, uint32_t axis, double width, bool upper) {
    if (plane.w <= 0 || width >= 1)
        return true;
    const double component = (&plane.x)[axis];
    // A nonempty half-space includes its liquid-side corner. It can be wholly
    // contained only by a slab touching that corner; its maximum extent along
    // this axis is alpha/abs(normal). Do not construct 1-width, which rounds to
    // one for sub-ulp sweeps and loses exact complete-drain ownership transfer.
    return upper != (component >= 0) && plane.w <= fabs(component) * width;
}
__host__ __device__ inline uint32_t index(uint3 p, uint3 grid) {
    return (p.z * grid.y + p.y) * grid.x + p.x;
}
__host__ __device__ inline uint3 coordinate(uint32_t i, uint3 grid) {
    return make_uint3(i % grid.x, (i / grid.x) % grid.y, i / (grid.x * grid.y));
}
__host__ __device__ inline double3 widths(uint3 p, uint3 fine, double h) {
    return make_double3((2 * p.x + 1 < fine.x ? 2 : 1) * h, (2 * p.y + 1 < fine.y ? 2 : 1) * h,
                        (2 * p.z + 1 < fine.z ? 2 : 1) * h);
}
// phase.x is total liquid volume (both ownership representations); phase.y is
// geometric capacity. Ownership fraction alone must not define an air interface.
__device__ inline double4 reconstruct(uint32_t id, uint3 grid, uint3 fine, double h, const double2 *phase) {
    const auto p = coordinate(id, grid);
    const auto w = widths(p, fine, h);
    const double f = fmin(1., fmax(0., phase[id].x / phase[id].y));
    if (f == 0 || f == 1)
        return make_double4(0, 0, 0, f);
    double3 n = make_double3(0, 0, 0);
    for (uint32_t a = 0; a < 3; ++a) {
        double value[2] = {f, f}, distance = 0;
        for (uint32_t side = 0; side < 2; ++side) {
            uint3 q = p;
            if (side)
                (&q.x)[a]++;
            else
                (&q.x)[a]--;
            if ((&q.x)[a] >= (&grid.x)[a])
                continue;
            const auto neighbor = phase[index(q, grid)];
            value[side] = neighbor.x / neighbor.y;
            const auto nw = widths(q, fine, h);
            distance += ((&w.x)[a] + (&nw.x)[a]) * .5;
        }
        if (distance > 0)
            (&n.x)[a] = (value[0] - value[1]) * (&w.x)[a] / distance;
    }
    const double scale = fabs(n.x) + fabs(n.y) + fabs(n.z);
    if (!scale)
        return make_double4(0, 0, 0, f);
    n.x /= scale;
    n.y /= scale;
    n.z /= scale;
    return make_double4(n.x, n.y, n.z, intercept(n, f));
}
} // namespace lab::cuda_fluid::geometry
