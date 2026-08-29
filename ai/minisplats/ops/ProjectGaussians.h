// mini/ops/ProjectGaussians.h — 3D Gaussian projection to 2D (forward + backward).
// Forward ported from rasterizer/gsplat-cpu/gsplat_cpu.cpp (rewritten as per-point loops).
// Backward ported from rasterizer/gsplat/backward.cu + helpers.cuh (converted from
// glm column-major to row-major C).
//
// Uses explicit forward/backward (no autograd) because the projection has multiple
// outputs (xys, conics, camDepths, radii, cov2d). The caller manually invokes
// backward after autograd has filled the output gradients.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_OPS_PROJECT_GAUSSIANS_H
#define MINI_OPS_PROJECT_GAUSSIANS_H

#include "../Tensor.h"
#include "../Geometry.h"
#include <cmath>

namespace mini::ops {

// Context holding forward intermediates needed for backward.
struct ProjCtx {
    int N;
    float fx, fy, cx, cy;
    int imgWidth, imgHeight;
    float globScale;
    // View matrix (4x4 row-major) and proj matrix (4x4 row-major)
    std::vector<float> viewmat; // 16
    std::vector<float> projmat; // 16
    // Per-point intermediates
    std::vector<float> pView;    // [N*3] view-space position
    std::vector<float> cov3d;    // [N*6] (xx, xy, xz, yy, yz, zz)
    std::vector<float> cov2d;    // [N*4] (xx, xy, yx, yy)
    std::vector<float> conics;   // [N*3] (A, B, C)
    std::vector<int32_t> radii;  // [N]
    std::vector<float> xys;      // [N*2]
    std::vector<float> camDepths;// [N]
    // Input data (for backward)
    std::vector<float> means_data;    // [N*3]
    std::vector<float> scales_data;   // [N*3] (already exp'd)
    std::vector<float> quats_data;    // [N*4] (already normalized)
};

struct ProjResult {
    Tensor xys;       // [N, 2]
    Tensor conics;    // [N, 3]
    Tensor cov2d;     // [N, 4] (row-major 2x2: xx, xy, yx, yy)
    Tensor radii;     // [N] int32 stored as float
    Tensor camDepths; // [N]
    ProjCtx ctx;
};

struct ProjBackwardResult {
    Tensor v_means;   // [N, 3]
    Tensor v_scales;  // [N, 3] (w.r.t. exp'd scales)
    Tensor v_quats;   // [N, 4] (w.r.t. normalized quats)
};

// ---- Small matrix helpers (row-major) ----
inline void mat3_mul(const float* A, const float* B, float* C) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += A[i * 3 + k] * B[k * 3 + j];
            C[i * 3 + j] = s;
        }
}
inline void mat3_transpose(const float* A, float* At) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            At[j * 3 + i] = A[i * 3 + j];
}
// 2x3 @ 3x3 -> 2x3
inline void mat23_mul_mat33(const float* A, const float* B, float* C) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += A[i * 3 + k] * B[k * 3 + j];
            C[i * 3 + j] = s;
        }
}
// 2x3 @ 3x2 -> 2x2
inline void mat23_mul_mat32(const float* A, const float* B, float* C) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += A[i * 3 + k] * B[k * 2 + j];
            C[i * 2 + j] = s;
        }
}
// 2x2 @ 2x3 -> 2x3
inline void mat22_mul_mat23(const float* A, const float* B, float* C) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 2; k++) s += A[i * 2 + k] * B[k * 3 + j];
            C[i * 3 + j] = s;
        }
}
// 3x2 @ 2x2 -> 3x2
inline void mat32_mul_mat22(const float* A, const float* B, float* C) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 2; j++) {
            float s = 0.0f;
            for (int k = 0; k < 2; k++) s += A[i * 2 + k] * B[k * 2 + j];
            C[i * 2 + j] = s;
        }
}
// 2x2 @ 2x2 -> 2x2
inline void mat22_mul(const float* A, const float* B, float* C) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float s = 0.0f;
            for (int k = 0; k < 2; k++) s += A[i * 2 + k] * B[k * 2 + j];
            C[i * 2 + j] = s;
        }
}

// ---- Forward ----
inline ProjResult projectGaussiansForward(
    const Tensor& means,    // [N, 3]
    const Tensor& scales,   // [N, 3] (already exp'd)
    float globScale,
    const Tensor& quats,    // [N, 4] (already normalized)
    const Tensor& viewmat,  // [4, 4] row-major
    const Tensor& projmat,  // [4, 4] row-major
    float fx, float fy, float cx, float cy,
    int imgHeight, int imgWidth,
    float clipThresh = 0.01f)
{
    int N = (int)means.size(0);
    ProjResult result;
    ProjCtx& ctx = result.ctx;
    ctx.N = N;
    ctx.fx = fx; ctx.fy = fy; ctx.cx = cx; ctx.cy = cy;
    ctx.imgWidth = imgWidth; ctx.imgHeight = imgHeight;
    ctx.globScale = globScale;
    ctx.viewmat.assign(viewmat.data_ptr(), viewmat.data_ptr() + 16);
    ctx.projmat.assign(projmat.data_ptr(), projmat.data_ptr() + 16);
    ctx.pView.resize(N * 3);
    ctx.cov3d.resize(N * 6);
    ctx.cov2d.resize(N * 4);
    ctx.conics.resize(N * 3);
    ctx.radii.resize(N);
    ctx.xys.resize(N * 2);
    ctx.camDepths.resize(N);
    ctx.means_data.assign(means.data_ptr(), means.data_ptr() + N * 3);
    ctx.scales_data.assign(scales.data_ptr(), scales.data_ptr() + N * 3);
    ctx.quats_data.assign(quats.data_ptr(), quats.data_ptr() + N * 4);

    float fovX = 0.5f * imgWidth / fx;
    float fovY = 0.5f * imgHeight / fy;
    float limX = 1.3f * fovX;
    float limY = 1.3f * fovY;

    const float* vm = ctx.viewmat.data();
    // Rclip = viewmat[:3, :3], Tclip = viewmat[:3, 3]
    float Rclip[9] = {vm[0], vm[1], vm[2], vm[4], vm[5], vm[6], vm[8], vm[9], vm[10]};
    float Tclip[3] = {vm[3], vm[7], vm[11]};

    for (int n = 0; n < N; n++) {
        const float* m = &ctx.means_data[n * 3];
        float* pV = &ctx.pView[n * 3];

        // pView = Rclip @ means + Tclip
        pV[0] = Rclip[0] * m[0] + Rclip[1] * m[1] + Rclip[2] * m[2] + Tclip[0];
        pV[1] = Rclip[3] * m[0] + Rclip[4] * m[1] + Rclip[5] * m[2] + Tclip[1];
        pV[2] = Rclip[6] * m[0] + Rclip[7] * m[1] + Rclip[8] * m[2] + Tclip[2];

        // quatToRotMat (inlined; same as Geometry.h quatToRotMat)
        float R[9];
        {
            const float* q = &ctx.quats_data[n * 4];
            float w = q[0], x = q[1], y = q[2], z = q[3];
            float nn = std::sqrt(w * w + x * x + y * y + z * z);
            w /= nn; x /= nn; y /= nn; z /= nn;
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

        // M = R * globScale * diag(scales)
        // M[i,j] = R[i,j] * globScale * scales[j]
        const float* sc = &ctx.scales_data[n * 3];
        float M[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M[i * 3 + j] = R[i * 3 + j] * globScale * sc[j];

        // cov3d = M @ M^T (symmetric, store 6: xx, xy, xz, yy, yz, zz)
        float cov[9];
        float Mt[9];
        mat3_transpose(M, Mt);
        mat3_mul(M, Mt, cov);
        ctx.cov3d[n * 6 + 0] = cov[0]; // xx
        ctx.cov3d[n * 6 + 1] = cov[1]; // xy
        ctx.cov3d[n * 6 + 2] = cov[2]; // xz
        ctx.cov3d[n * 6 + 3] = cov[4]; // yy
        ctx.cov3d[n * 6 + 4] = cov[5]; // yz
        ctx.cov3d[n * 6 + 5] = cov[8]; // zz

        // EWA projection
        // Clamp pView/pView.z ratio
        float t0 = pV[0], t1 = pV[1], t2 = pV[2];
        if (t2 < clipThresh) t2 = clipThresh; // avoid div by zero
        float r0 = t0 / t2, r1 = t1 / t2;
        r0 = std::min(limX, std::max(-limX, r0));
        r1 = std::min(limY, std::max(-limY, r1));
        t0 = r0 * t2; t1 = r1 * t2;

        float rz = 1.0f / t2;
        float rz2 = rz * rz;

        // J = [[fx*rz, 0, 0], [0, fy*rz, 0], [-fx*t0*rz2, -fy*t1*rz2, 0]]
        float J[6] = {
            fx * rz, 0.0f, 0.0f,
            0.0f, fy * rz, 0.0f
        };

        // T = J @ Rclip (2x3)
        float T[6];
        mat23_mul_mat33(J, Rclip, T);

        // cov2d = T @ cov3d @ T^T (2x2)
        // First: cov3d_as_mat3 (symmetric)
        float V[9] = {
            cov[0], cov[1], cov[2],
            cov[1], cov[4], cov[5],
            cov[2], cov[5], cov[8]
        };
        // T @ V (2x3)
        float TV[6];
        mat23_mul_mat32(T, V, TV); // Wait, V is 3x3, T is 2x3. T @ V is 2x3.
        // Actually mat23_mul_mat33(T, V, TV) gives 2x3
        mat23_mul_mat33(T, V, TV);
        // TV @ T^T (2x3 @ 3x2 -> 2x2)
        float Tt[6]; // T^T is 3x2
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 2; j++)
                Tt[i * 2 + j] = T[j * 3 + i];
        float cov2d[4];
        mat23_mul_mat32(TV, Tt, cov2d);

        // Add blur
        cov2d[0] += 0.3f;
        cov2d[3] += 0.3f;

        ctx.cov2d[n * 4 + 0] = cov2d[0];
        ctx.cov2d[n * 4 + 1] = cov2d[1];
        ctx.cov2d[n * 4 + 2] = cov2d[2];
        ctx.cov2d[n * 4 + 3] = cov2d[3];

        // conics = inverse(cov2d)
        float det = cov2d[0] * cov2d[3] - cov2d[1] * cov2d[2];
        det = std::max(det, 1e-6f);
        ctx.conics[n * 3 + 0] = cov2d[3] / det;  // A
        ctx.conics[n * 3 + 1] = -cov2d[1] / det; // B
        ctx.conics[n * 3 + 2] = cov2d[0] / det;  // C

        // radius
        float b = (cov2d[0] + cov2d[3]) * 0.5f;
        float sq = std::sqrt(std::max(b * b - det, 0.1f));
        float v1 = b + sq, v2 = b - sq;
        ctx.radii[n] = (int32_t)std::ceil(3.0f * std::sqrt(std::max(v1, v2)));

        // NDC projection of means
        float pHom[4];
        pHom[0] = ctx.projmat[0] * m[0] + ctx.projmat[1] * m[1] + ctx.projmat[2] * m[2] + ctx.projmat[3];
        pHom[1] = ctx.projmat[4] * m[0] + ctx.projmat[5] * m[1] + ctx.projmat[6] * m[2] + ctx.projmat[7];
        pHom[2] = ctx.projmat[8] * m[0] + ctx.projmat[9] * m[1] + ctx.projmat[10] * m[2] + ctx.projmat[11];
        pHom[3] = ctx.projmat[12] * m[0] + ctx.projmat[13] * m[1] + ctx.projmat[14] * m[2] + ctx.projmat[15];
        float rw = 1.0f / std::max(pHom[3], 1e-6f);
        float pProjX = pHom[0] * rw;
        float pProjY = pHom[1] * rw;
        float pProjZ = pHom[2] * rw;
        float u = 0.5f * ((pProjX + 1.0f) * imgWidth - 1.0f);
        float v = 0.5f * ((pProjY + 1.0f) * imgHeight - 1.0f);
        ctx.xys[n * 2 + 0] = u;
        ctx.xys[n * 2 + 1] = v;
        ctx.camDepths[n] = pProjZ;
    }

    // Create output tensors
    result.xys = Tensor::from_blob(ctx.xys.data(), {N, 2});
    result.conics = Tensor::from_blob(ctx.conics.data(), {N, 3});
    result.cov2d = Tensor::from_blob(ctx.cov2d.data(), {N, 4});
    result.radii = Tensor::from_blob((float*)ctx.radii.data(), {N});
    result.camDepths = Tensor::from_blob(ctx.camDepths.data(), {N});

    return result;
}

// ---- Backward ----
// Ported from rasterizer/gsplat/backward.cu project_gaussians_backward_kernel +
// helpers.cuh (project_pix_vjp, cov2d_to_conic_vjp, project_cov3d_ewa_vjp,
// scale_rot_to_cov3d_vjp, quat_to_rotmat_vjp). Converted from glm column-major
// to row-major C.
inline ProjBackwardResult projectGaussiansBackward(
    const ProjCtx& ctx,
    const Tensor& v_xys,      // [N, 2]
    const Tensor& v_conics,   // [N, 3]
    const Tensor& v_depths)   // [N]
{
    int N = ctx.N;
    auto result = ProjBackwardResult{
        Tensor::zeros({N, 3}),  // v_means
        Tensor::zeros({N, 3}),  // v_scales (w.r.t. exp'd scales)
        Tensor::zeros({N, 4})   // v_quats
    };

    const float* vm = ctx.viewmat.data();
    const float* pm = ctx.projmat.data();
    float fx = ctx.fx, fy = ctx.fy;
    int W = ctx.imgWidth, H = ctx.imgHeight;

    // Rclip (3x3 row-major) and Tclip
    float Rclip[9] = {vm[0], vm[1], vm[2], vm[4], vm[5], vm[6], vm[8], vm[9], vm[10]};
    float RclipT[9];
    mat3_transpose(Rclip, RclipT);

    for (int n = 0; n < N; n++) {
        if (ctx.radii[n] <= 0) continue;

        const float* mean = &ctx.means_data[n * 3];
        const float* sc = &ctx.scales_data[n * 3];
        const float* q = &ctx.quats_data[n * 4];
        const float* cov3d = &ctx.cov3d[n * 6];
        const float* conic = &ctx.conics[n * 3];

        float v_xy[2] = {v_xys.data_ptr()[n * 2], v_xys.data_ptr()[n * 2 + 1]};
        float v_conic[3] = {v_conics.data_ptr()[n * 3], v_conics.data_ptr()[n * 3 + 1], v_conics.data_ptr()[n * 3 + 2]};
        float v_depth = v_depths.data_ptr()[n];

        float v_mean[3] = {0, 0, 0};
        float v_scale[3] = {0, 0, 0};
        float v_quat[4] = {0, 0, 0, 0};

        // 1. project_pix_vjp: v_mean from v_xy
        {
            float pHom[4];
            pHom[0] = pm[0] * mean[0] + pm[1] * mean[1] + pm[2] * mean[2] + pm[3];
            pHom[1] = pm[4] * mean[0] + pm[5] * mean[1] + pm[6] * mean[2] + pm[7];
            pHom[3] = pm[12] * mean[0] + pm[13] * mean[1] + pm[14] * mean[2] + pm[15];
            float rw = 1.0f / (pHom[3] + 1e-6f);
            float v_ndc_x = 0.5f * W * v_xy[0];
            float v_ndc_y = 0.5f * H * v_xy[1];
            float v_proj[4] = {v_ndc_x * rw, v_ndc_y * rw, 0.0f, -(v_ndc_x + v_ndc_y) * rw * rw};
            // v_world = P[:3,:3]^T @ v_proj[:3]
            v_mean[0] = pm[0] * v_proj[0] + pm[4] * v_proj[1] + pm[8] * v_proj[2];
            v_mean[1] = pm[1] * v_proj[0] + pm[5] * v_proj[1] + pm[9] * v_proj[2];
            v_mean[2] = pm[2] * v_proj[0] + pm[6] * v_proj[1] + pm[10] * v_proj[2];
        }

        // 2. z gradient contribution
        v_mean[0] += vm[8] * v_depth;
        v_mean[1] += vm[9] * v_depth;
        v_mean[2] += vm[10] * v_depth;

        // 3. cov2d_to_conic_vjp: v_cov2d from v_conic
        // X = conic matrix [[A, B], [B, C]], G = v_conic matrix
        // v_Sigma = -X * G * X
        float X[4] = {conic[0], conic[1], conic[1], conic[2]};
        float G[4] = {v_conic[0], v_conic[1], v_conic[1], v_conic[2]};
        float XG[4], vS[4];
        mat22_mul(X, G, XG);
        mat22_mul(XG, X, vS);
        float v_cov2d[3]; // (xx, xy, yy) upper triangular
        v_cov2d[0] = -vS[0];
        v_cov2d[1] = -(vS[1] + vS[2]); // off-diagonal summed
        v_cov2d[2] = -vS[3];

        // 4. project_cov3d_ewa_vjp: v_cov3d and additional v_mean
        {
            // W = Rclip (3x3), p = Tclip
            // t = W @ mean + p
            float t[3];
            t[0] = Rclip[0] * mean[0] + Rclip[1] * mean[1] + Rclip[2] * mean[2] + vm[3];
            t[1] = Rclip[3] * mean[0] + Rclip[4] * mean[1] + Rclip[5] * mean[2] + vm[7];
            t[2] = Rclip[6] * mean[0] + Rclip[7] * mean[1] + Rclip[8] * mean[2] + vm[11];
            float rz = 1.0f / t[2];
            float rz2 = rz * rz;
            float rz3 = rz2 * rz;

            // J (3x3, only top 2 rows matter)
            float J[9] = {
                fx * rz, 0.0f, 0.0f,
                0.0f, fy * rz, 0.0f,
                -fx * t[0] * rz2, -fy * t[1] * rz2, 0.0f
            };
            // T = J @ W (3x3)
            float T[9];
            mat3_mul(J, Rclip, T);
            float Tt[9];
            mat3_transpose(T, Tt);

            // V = cov3d as 3x3 symmetric
            float V[9] = {
                cov3d[0], cov3d[1], cov3d[2],
                cov3d[1], cov3d[3], cov3d[4],
                cov3d[2], cov3d[4], cov3d[5]
            };
            float Vt[9];
            mat3_transpose(V, Vt);

            // v_cov (3x3) from v_cov2d (padded)
            float v_cov[9] = {
                v_cov2d[0], 0.5f * v_cov2d[1], 0.0f,
                0.5f * v_cov2d[1], v_cov2d[2], 0.0f,
                0.0f, 0.0f, 0.0f
            };
            float v_covT[9];
            mat3_transpose(v_cov, v_covT);

            // v_V = T^T @ v_cov @ T (3x3)
            float tmp[9], v_V[9];
            mat3_mul(Tt, v_cov, tmp);
            mat3_mul(tmp, T, v_V);

            // v_T = v_cov @ T @ V^T + v_cov^T @ T @ V (3x3)
            float tmp1[9], tmp2[9], tmp3[9], v_T[9];
            mat3_mul(v_cov, T, tmp1);
            mat3_mul(tmp1, Vt, tmp2);
            mat3_mul(v_covT, T, tmp3);
            mat3_mul(tmp3, V, tmp1);
            for (int i = 0; i < 9; i++) v_T[i] = tmp2[i] + tmp1[i];

            // v_cov3d (6 values)
            result.v_scales.data_ptr(); // no-op
            // v_cov3d[0]=v_V[0], [1]=v_V[1]+v_V[3], [2]=v_V[2]+v_V[6], [3]=v_V[4], [4]=v_V[5]+v_V[7], [5]=v_V[8]
            float v_cov3d[6];
            v_cov3d[0] = v_V[0];
            v_cov3d[1] = v_V[1] + v_V[3];
            v_cov3d[2] = v_V[2] + v_V[6];
            v_cov3d[3] = v_V[4];
            v_cov3d[4] = v_V[5] + v_V[7];
            v_cov3d[5] = v_V[8];

            // v_J = v_T @ W^T (3x3)
            float v_J[9];
            mat3_mul(v_T, RclipT, v_J);

            // v_t from v_J (derivative of J w.r.t. t)
            float v_t[3];
            v_t[0] = -fx * rz2 * v_J[6]; // v_J[2][0]
            v_t[1] = -fy * rz2 * v_J[7]; // v_J[2][1]
            v_t[2] = -fx * rz2 * v_J[0] + 2.0f * fx * t[0] * rz3 * v_J[6]
                     - fy * rz2 * v_J[4] + 2.0f * fy * t[1] * rz3 * v_J[7];
            // v_J[0] = [0][0], v_J[4] = [1][1], v_J[6] = [2][0], v_J[7] = [2][1]

            // v_mean += W^T @ v_t
            v_mean[0] += Rclip[0] * v_t[0] + Rclip[3] * v_t[1] + Rclip[6] * v_t[2];
            v_mean[1] += Rclip[1] * v_t[0] + Rclip[4] * v_t[1] + Rclip[7] * v_t[2];
            v_mean[2] += Rclip[2] * v_t[0] + Rclip[5] * v_t[1] + Rclip[8] * v_t[2];

            // 5. scale_rot_to_cov3d_vjp: v_scale, v_quat from v_cov3d
            {
                // Reconstruct R from quat
                float R[9];
                {
                    float w = q[0], x = q[1], y = q[2], z = q[3];
                    float nn = std::sqrt(w * w + x * x + y * y + z * z);
                    w /= nn; x /= nn; y /= nn; z /= nn;
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
                // S = diag(globScale * scale)
                float S0 = ctx.globScale * sc[0];
                float S1 = ctx.globScale * sc[1];
                float S2 = ctx.globScale * sc[2];
                // M = R @ S -> M[i,j] = R[i,j] * S[j]
                float M[9];
                M[0] = R[0] * S0; M[1] = R[1] * S1; M[2] = R[2] * S2;
                M[3] = R[3] * S0; M[4] = R[4] * S1; M[5] = R[5] * S2;
                M[6] = R[6] * S0; M[7] = R[7] * S1; M[8] = R[8] * S2;

                // v_V (symmetric from v_cov3d)
                float v_V_sym[9] = {
                    v_cov3d[0], 0.5f * v_cov3d[1], 0.5f * v_cov3d[2],
                    0.5f * v_cov3d[1], v_cov3d[3], 0.5f * v_cov3d[4],
                    0.5f * v_cov3d[2], 0.5f * v_cov3d[4], v_cov3d[5]
                };
                // v_M = 2 * v_V_sym @ M
                float v_M[9];
                mat3_mul(v_V_sym, M, v_M);
                for (int i = 0; i < 9; i++) v_M[i] *= 2.0f;

                // v_scale[d] = dot(R[:,d], v_M[:,d]) = sum_i R[i,d] * v_M[i,d]
                // In row-major: R[i*3+d], v_M[i*3+d]
                v_scale[0] = R[0] * v_M[0] + R[3] * v_M[3] + R[6] * v_M[6];
                v_scale[1] = R[1] * v_M[1] + R[4] * v_M[4] + R[7] * v_M[7];
                v_scale[2] = R[2] * v_M[2] + R[5] * v_M[5] + R[8] * v_M[8];

                // v_R = v_M @ S (v_R[i,j] = v_M[i,j] * S[j])
                float v_R[9];
                for (int i = 0; i < 3; i++) {
                    v_R[i * 3 + 0] = v_M[i * 3 + 0] * S0;
                    v_R[i * 3 + 1] = v_M[i * 3 + 1] * S1;
                    v_R[i * 3 + 2] = v_M[i * 3 + 2] * S2;
                }

                // quat_to_rotmat_vjp: convert v_R (row-major) to v_quat
                // Ported from helpers.cuh, converting glm col-major to row-major.
                // In the GPU code: v_R[col][row] in glm → our v_R[row*3+col].
                // quat mapping: q[0]=w, q[1]=x, q[2]=y, q[3]=z
                float w = q[0], x = q[1], y = q[2], z = q[3];
                float nn = std::sqrt(w * w + x * x + y * y + z * z);
                w /= nn; x /= nn; y /= nn; z /= nn;
                // glm indices → row-major:
                // v_R[1][2] → R[2*3+1]=R[7], v_R[2][1] → R[1*3+2]=R[5]
                // v_R[2][0] → R[0*3+2]=R[2], v_R[0][2] → R[2*3+0]=R[6]
                // v_R[0][1] → R[1*3+0]=R[3], v_R[1][0] → R[0*3+1]=R[1]
                // v_R[1][1] → R[4], v_R[2][2] → R[8], v_R[0][0] → R[0]
                v_quat[0] = 2.0f * (x * (v_R[7] - v_R[5]) + y * (v_R[2] - v_R[6]) + z * (v_R[3] - v_R[1]));
                v_quat[1] = 2.0f * (-2.0f * x * (v_R[4] + v_R[8]) + y * (v_R[3] + v_R[1]) + z * (v_R[6] + v_R[2]) + w * (v_R[7] - v_R[5]));
                v_quat[2] = 2.0f * (x * (v_R[3] + v_R[1]) - 2.0f * y * (v_R[0] + v_R[8]) + z * (v_R[7] + v_R[5]) + w * (v_R[2] - v_R[6]));
                v_quat[3] = 2.0f * (x * (v_R[6] + v_R[2]) + y * (v_R[7] + v_R[5]) - 2.0f * z * (v_R[0] + v_R[4]) + w * (v_R[3] - v_R[1]));
            }
        }

        // Write grads
        result.v_means.data_ptr()[n * 3 + 0] = v_mean[0];
        result.v_means.data_ptr()[n * 3 + 1] = v_mean[1];
        result.v_means.data_ptr()[n * 3 + 2] = v_mean[2];
        result.v_scales.data_ptr()[n * 3 + 0] = v_scale[0];
        result.v_scales.data_ptr()[n * 3 + 1] = v_scale[1];
        result.v_scales.data_ptr()[n * 3 + 2] = v_scale[2];
        result.v_quats.data_ptr()[n * 4 + 0] = v_quat[0];
        result.v_quats.data_ptr()[n * 4 + 1] = v_quat[1];
        result.v_quats.data_ptr()[n * 4 + 2] = v_quat[2];
        result.v_quats.data_ptr()[n * 4 + 3] = v_quat[3];
    }
    return result;
}

} // namespace mini::ops

#endif // MINI_OPS_PROJECT_GAUSSIANS_H
