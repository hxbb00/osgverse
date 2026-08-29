// mini/Geometry.h — Quaternion / rotation / projection-matrix utilities.
// Standalone (no Eigen dependency); replaces tensor_math.cpp + parts of model.cpp.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_GEOMETRY_H
#define MINI_GEOMETRY_H

#include "Tensor.h"
#include <cmath>

namespace mini {

constexpr float PI = 3.14159265358979323846f;

// Random unit quaternion (uniform on S^3). Matches model.cpp randomQuatTensor.
inline Tensor randomQuatTensor(int64_t n) {
    auto u = Tensor::uniform({n}, 0.0f, 1.0f);
    auto v = Tensor::uniform({n}, 0.0f, 1.0f);
    auto w = Tensor::uniform({n}, 0.0f, 1.0f);
    auto p = Tensor::empty({n, 4});
    for (int64_t i = 0; i < n; i++) {
        float ui = u.data_ptr()[i], vi = v.data_ptr()[i], wi = w.data_ptr()[i];
        p.data_ptr()[i * 4 + 0] = std::sqrt(1.0f - ui) * std::sin(2.0f * PI * vi);
        p.data_ptr()[i * 4 + 1] = std::sqrt(1.0f - ui) * std::cos(2.0f * PI * vi);
        p.data_ptr()[i * 4 + 2] = std::sqrt(ui) * std::sin(2.0f * PI * wi);
        p.data_ptr()[i * 4 + 3] = std::sqrt(ui) * std::cos(2.0f * PI * wi);
    }
    return p;
}

// Quaternion [w,x,y,z] -> 3x3 rotation matrix (row-major).
// Matches tensor_math.cpp quatToRotMat. Expects normalized quaternion.
inline void quatToRotMat(const float* q, float* R) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    // normalize
    float n = std::sqrt(w * w + x * x + y * y + z * z);
    w /= n; x /= n; y /= n; z /= n;
    R[0] = 1.0f - 2.0f * (y * y + z * z);
    R[1] = 2.0f * (x * y - w * z);
    R[2] = 2.0f * (x * z + w * y);
    R[3] = 2.0f * (x * y + w * z);
    R[4] = 1.0f - 2.0f * (x * x + z * z);
    R[5] = 2.0f * (y * z - w * x);
    R[6] = 2.0f * (x * z - w * y);
    R[7] = 2.0f * (y * z + w * x);
    R[8] = 1.0f - 2.0f * (x * x + y * y);
}

// Normalize quaternions to unit length.  Input: [N, 4].  Output: [N, 4].
inline Tensor normalizeQuats(const Tensor& quats) {
    int64_t n = quats.size(0);
    Tensor out = Tensor::empty({n, 4});
    for (int64_t i = 0; i < n; i++) {
        const float* q = quats.data_ptr() + i * 4;
        float* o = out.data_ptr() + i * 4;
        float len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (len < 1e-12f) len = 1e-12f;
        float inv = 1.0f / len;
        o[0] = q[0] * inv; o[1] = q[1] * inv; o[2] = q[2] * inv; o[3] = q[3] * inv;
    }
    return out;
}

// OpenGL perspective projection matrix (4x4, row-major).
// Matches model.cpp projectionMatrix.
inline Tensor projectionMatrix(float zNear, float zFar, float fovX, float fovY) {
    float t = zNear * std::tan(0.5f * fovY);
    float b = -t;
    float r = zNear * std::tan(0.5f * fovX);
    float l = -r;
    float m[16] = {
        2.0f * zNear / (r - l), 0.0f, (r + l) / (r - l), 0.0f,
        0.0f, 2.0f * zNear / (t - b), (t + b) / (t - b), 0.0f,
        0.0f, 0.0f, (zFar + zNear) / (zFar - zNear), -1.0f * zFar * zNear / (zFar - zNear),
        0.0f, 0.0f, 1.0f, 0.0f
    };
    return Tensor::from_blob(m, {4, 4});
}

// autoScaleAndCenterPoses: center camera origins at mean, scale to unit cube.
// Returns {poses, center, scale}. Matches tensor_math.cpp.
inline std::tuple<Tensor, Tensor, float> autoScaleAndCenterPoses(const Tensor& poses) {
    // poses: [N, 4, 4]; origins = poses[:, :3, 3]
    int64_t n = poses.size(0);
    // Compute mean origin
    float cx = 0, cy = 0, cz = 0;
    for (int64_t i = 0; i < n; i++) {
        cx += poses.at({i, 0, 3});
        cy += poses.at({i, 1, 3});
        cz += poses.at({i, 2, 3});
    }
    cx /= n; cy /= n; cz /= n;
    // Center
    Tensor centered = poses.contiguous();
    for (int64_t i = 0; i < n; i++) {
        centered.data_ptr()[i * 16 + 0 * 4 + 3] -= cx;
        centered.data_ptr()[i * 16 + 1 * 4 + 3] -= cy;
        centered.data_ptr()[i * 16 + 2 * 4 + 3] -= cz;
    }
    // Scale: 1 / max(abs(origins))
    float maxAbs = 0.0f;
    for (int64_t i = 0; i < n; i++) {
        maxAbs = std::max(maxAbs, std::fabs(centered.at({i, 0, 3})));
        maxAbs = std::max(maxAbs, std::fabs(centered.at({i, 1, 3})));
        maxAbs = std::max(maxAbs, std::fabs(centered.at({i, 2, 3})));
    }
    float scale = 1.0f / maxAbs;
    for (int64_t i = 0; i < n; i++) {
        centered.data_ptr()[i * 16 + 0 * 4 + 3] *= scale;
        centered.data_ptr()[i * 16 + 1 * 4 + 3] *= scale;
        centered.data_ptr()[i * 16 + 2 * 4 + 3] *= scale;
    }
    Tensor center = Tensor::from_blob(std::vector<float>{cx, cy, cz}.data(), {3});
    return {centered, center, scale};
}

// rgb2sh / sh2rgb: 0th-order SH <-> RGB.
constexpr float SH_C0 = 0.28209479177387814f;

inline Tensor rgb2sh(const Tensor& rgb) {
    // (rgb - 0.5) / C0
    return (rgb - 0.5f) * (1.0f / SH_C0);
}

inline Tensor sh2rgb(const Tensor& sh) {
    // clamp(sh * C0 + 0.5, 0, 1)
    return (sh * SH_C0 + 0.5f).clamp(0.0f, 1.0f);
}

} // namespace mini

#endif // MINI_GEOMETRY_H
