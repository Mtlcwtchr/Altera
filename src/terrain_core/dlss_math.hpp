#pragma once
#include <algorithm>
#include <cmath>

namespace terrain_core::dlss_math {
inline float halton(unsigned index, unsigned base) {
    float value = 0, fraction = 1;
    while (index) { fraction /= float(base); value += fraction * float(index % base); index /= base; }
    return value;
}

// Previous clip coordinates from this frame's jittered clip coordinates.
// Cameras have eye-relative matrices, so the eye displacement belongs between
// the inverse current matrix and the previous projection (in double precision).
inline bool reprojection(const float current[16], const float previous[16],
                         const double displacement[3], float output[16]) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = current[r * 4 + c];
        a[r][r + 4] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double divisor = a[c][c];
        for (double& v : a[c]) v /= divisor;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const double factor = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        double v = 0;
        for (int k = 0; k < 4; ++k) {
            const double world = a[k][c + 4] + (k < 3 ? displacement[k] * a[3][c + 4] : 0);
            v += previous[r * 4 + k] * world;
        }
        output[r * 4 + c] = float(v);
    }
    return true;
}
} // namespace terrain_core::dlss_math
