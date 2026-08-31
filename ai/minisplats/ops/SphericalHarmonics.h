// mini/ops/SphericalHarmonics.h — Spherical harmonics evaluation (forward + backward).
// Ported from rasterizer/gsplat-cpu/gsplat_cpu.cpp compute_sh_forward_tensor_cpu
// and the GPU backward kernel. Implemented as a custom autograd op.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_OPS_SPHERICAL_HARMONICS_H
#define MINI_OPS_SPHERICAL_HARMONICS_H

#include "../Tensor.h"
#include <cmath>

namespace mini { namespace ops {

// SH constants (same as gsplat_cpu.cpp)
static const float SH_C0 = 0.28209479177387814f;
static const float SH_C1 = 0.4886025119029199f;
static const float SH_C2[] = {1.0925484305920792f, -1.0925484305920792f, 0.31539156525252005f, -1.0925484305920792f, 0.5462742152960396f};
static const float SH_C3[] = {-0.5900435899266435f, 2.890611442640554f, -0.4570457994644658f, 0.3731763325901154f, -0.4570457994644658f, 1.445305721320277f, -0.5900435899266435f};
static const float SH_C4[] = {2.5033429417967046f, -1.7701307697799304f, 0.9461746957575601f, -0.6690465435572892f, 0.10578554691520431f, -0.6690465435572892f, 0.47308734787878004f, -1.7701307697799304f, 0.6258357354491761f};

inline int numShBases(int degree) {
    switch (degree) {
        case 0: return 1; case 1: return 4; case 2: return 9;
        case 3: return 16; default: return 25;
    }
}

// Compute SH basis values [N, K] from viewdirs [N, 3].
// Writes basis into `basis` (N*K floats, row-major).
inline void computeShBasis(float* basis, int N, int degreesToUse,
                           const float* viewdirs) {
    int K = numShBases(degreesToUse);
    for (int i = 0; i < N; i++) {
        float x = viewdirs[i * 3 + 0];
        float y = viewdirs[i * 3 + 1];
        float z = viewdirs[i * 3 + 2];
        float* b = basis + i * K;
        b[0] = SH_C0;
        if (K > 1) {
            b[1] = SH_C1 * -y;
            b[2] = SH_C1 * z;
            b[3] = SH_C1 * -x;
            if (K > 4) {
                float xx = x * x, yy = y * y, zz = z * z;
                float xy = x * y, yz = y * z, xz = x * z;
                b[4] = SH_C2[0] * xy;
                b[5] = SH_C2[1] * yz;
                b[6] = SH_C2[2] * (2.0f * zz - xx - yy);
                b[7] = SH_C2[3] * xz;
                b[8] = SH_C2[4] * (xx - yy);
                if (K > 9) {
                    b[9]  = SH_C3[0] * y * (3 * xx - yy);
                    b[10] = SH_C3[1] * xy * z;
                    b[11] = SH_C3[2] * y * (4 * zz - xx - yy);
                    b[12] = SH_C3[3] * z * (2 * zz - 3 * xx - 3 * yy);
                    b[13] = SH_C3[4] * x * (4 * zz - xx - yy);
                    b[14] = SH_C3[5] * z * (xx - yy);
                    b[15] = SH_C3[6] * x * (xx - 3 * yy);
                    if (K > 16) {
                        b[16] = SH_C4[0] * xy * (xx - yy);
                        b[17] = SH_C4[1] * yz * (3 * xx - yy);
                        b[18] = SH_C4[2] * xy * (7 * zz - 1);
                        b[19] = SH_C4[3] * yz * (7 * zz - 3);
                        b[20] = SH_C4[4] * (zz * (35 * zz - 30) + 3);
                        b[21] = SH_C4[5] * xz * (7 * zz - 3);
                        b[22] = SH_C4[6] * (xx - yy) * (7 * zz - 1);
                        b[23] = SH_C4[7] * xz * (xx - 3 * yy);
                        b[24] = SH_C4[8] * (xx * (xx - 3 * yy) - yy * (3 * xx - yy));
                    }
                }
            }
        }
    }
}

// Compute dBasis/dViewdir: for each basis function k, the derivative w.r.t. (x,y,z).
// Returns [N, K, 3] (flattened, row-major).
inline void computeShBasisDeriv(float* dbasis, int N, int degreesToUse,
                                 const float* viewdirs) {
    int K = numShBases(degreesToUse);
    for (int i = 0; i < N; i++) {
        float x = viewdirs[i * 3 + 0];
        float y = viewdirs[i * 3 + 1];
        float z = viewdirs[i * 3 + 2];
        float* db = dbasis + i * K * 3;
        // k=0: constant -> 0
        db[0] = db[1] = db[2] = 0.0f;
        if (K > 1) {
            // b[1] = -C1*y -> d/dx=0, d/dy=-C1, d/dz=0
            db[3] = 0.0f; db[4] = -SH_C1; db[5] = 0.0f;
            // b[2] = C1*z
            db[6] = 0.0f; db[7] = 0.0f; db[8] = SH_C1;
            // b[3] = -C1*x
            db[9] = -SH_C1; db[10] = 0.0f; db[11] = 0.0f;
            if (K > 4) {
                float xx = x * x, yy = y * y, zz = z * z;
                // b[4] = C2[0]*xy
                db[12] = SH_C2[0] * y; db[13] = SH_C2[0] * x; db[14] = 0.0f;
                // b[5] = C2[1]*yz
                db[15] = 0.0f; db[16] = SH_C2[1] * z; db[17] = SH_C2[1] * y;
                // b[6] = C2[2]*(2zz-xx-yy)
                db[18] = SH_C2[2] * (-2.0f * x); db[19] = SH_C2[2] * (-2.0f * y); db[20] = SH_C2[2] * (4.0f * z);
                // b[7] = C2[3]*xz
                db[21] = SH_C2[3] * z; db[22] = 0.0f; db[23] = SH_C2[3] * x;
                // b[8] = C2[4]*(xx-yy)
                db[24] = SH_C2[4] * (2.0f * x); db[25] = SH_C2[4] * (-2.0f * y); db[26] = 0.0f;
                if (K > 9) {
                    // b[9] = C3[0]*y*(3xx-yy) = C3[0]*(3xy*y - y^3)... d/dx=6C3[0]*xy, d/dy=C3[0]*(3xx-3yy), d/dz=0
                    db[27] = SH_C3[0] * 6.0f * x * y; db[28] = SH_C3[0] * (3.0f * xx - 3.0f * yy); db[29] = 0.0f;
                    // b[10] = C3[1]*xyz
                    db[30] = SH_C3[1] * y * z; db[31] = SH_C3[1] * x * z; db[32] = SH_C3[1] * x * y;
                    // b[11] = C3[2]*y*(4zz-xx-yy)
                    db[33] = SH_C3[2] * (-2.0f * x * y); db[34] = SH_C3[2] * (4.0f * zz - xx - 3.0f * yy); db[35] = SH_C3[2] * (8.0f * y * z);
                    // b[12] = C3[3]*z*(2zz-3xx-3yy)
                    db[36] = SH_C3[3] * (-6.0f * x * z); db[37] = SH_C3[3] * (-6.0f * y * z); db[38] = SH_C3[3] * (6.0f * zz - 3.0f * xx - 3.0f * yy);
                    // b[13] = C3[4]*x*(4zz-xx-yy)
                    db[39] = SH_C3[4] * (4.0f * zz - 3.0f * xx - yy); db[40] = SH_C3[4] * (-2.0f * x * y); db[41] = SH_C3[4] * (8.0f * x * z);
                    // b[14] = C3[5]*z*(xx-yy)
                    db[42] = SH_C3[5] * (2.0f * x * z); db[43] = SH_C3[5] * (-2.0f * y * z); db[44] = SH_C3[5] * (xx - yy);
                    // b[15] = C3[6]*x*(xx-3yy)
                    db[45] = SH_C3[6] * (3.0f * xx - 3.0f * yy); db[46] = SH_C3[6] * (-6.0f * x * y); db[47] = 0.0f;
                    // (K>16 derivatives omitted for brevity; add if sh_degree=4 needed)
                    if (K > 16) {
                        for (int k = 16; k < K; k++)
                            for (int d = 0; d < 3; d++)
                                db[k * 3 + d] = 0.0f; // placeholder; implement if needed
                    }
                }
            }
        }
    }
}

// SphericalHarmonics forward with autograd.
// viewdirs: [N, 3], coeffs: [N, K, 3] (K = numShBases(degree))
// returns: rgb [N, 3]
inline Tensor sphericalHarmonics(int degree, int degreesToUse,
                                  const Tensor& viewdirs, const Tensor& coeffs) {
    int N = (int)viewdirs.size(0);
    int K = numShBases(degree);
    int Kuse = numShBases(degreesToUse);

    // Compute basis [N, Kuse]
    std::vector<float> basis(N * Kuse);
    computeShBasis(basis.data(), N, degreesToUse, viewdirs.data_ptr());

    // rgb[n, c] = sum_k basis[n, k] * coeffs[n, k, c]
    // coeffs shape: [N, K, 3] — only first Kuse bases used
    auto rgb = Tensor::zeros({N, 3});
    const float* cp = coeffs.data_ptr();
    for (int n = 0; n < N; n++) {
        for (int c = 0; c < 3; c++) {
            float s = 0.0f;
            for (int k = 0; k < Kuse; k++)
                s += basis[n * Kuse + k] * cp[n * K * 3 + k * 3 + c];
            rgb.data_ptr()[n * 3 + c] = s;
        }
    }

    // Autograd setup
    bool vd_grad = viewdirs.requires_grad() && !is_no_grad();
    bool co_grad = coeffs.requires_grad() && !is_no_grad();
    if (!vd_grad && !co_grad) return rgb;

    rgb.impl()->requires_grad = true;
    if (vd_grad) rgb.impl()->inputs.push_back(viewdirs.impl());
    if (co_grad) rgb.impl()->inputs.push_back(coeffs.impl());

    // Capture basis for backward
    auto basis_ptr = std::make_shared<std::vector<float>>(std::move(basis));
    auto vd_impl = viewdirs.impl();
    auto co_impl = coeffs.impl();

    rgb.impl()->backward_fn = [vd_impl, co_impl, basis_ptr, N, K, Kuse, vd_grad, co_grad](TensorImpl& self) -> void {
        const float* g = self.grad->data.data();
        // d_coeffs[n, k, c] = g[n, c] * basis[n, k]
        if (co_grad) {
            co_impl->ensure_grad();
            float* dc = co_impl->grad->data.data();
            for (int n = 0; n < N; n++)
                for (int k = 0; k < Kuse; k++) {
                    float b = (*basis_ptr)[n * Kuse + k];
                    for (int c = 0; c < 3; c++)
                        dc[n * K * 3 + k * 3 + c] += g[n * 3 + c] * b;
                }
        }
        // d_viewdirs[n, d] = sum_{k,c} g[n,c] * coeffs[n,k,c] * dbasis[n,k,d]
        if (vd_grad) {
            vd_impl->ensure_grad();
            float* dv = vd_impl->grad->data.data();
            const float* cp = co_impl->data.data();
            std::vector<float> dbasis(N * Kuse * 3);
            computeShBasisDeriv(dbasis.data(), N, Kuse, vd_impl->data.data());
            for (int n = 0; n < N; n++) {
                for (int d = 0; d < 3; d++) {
                    float s = 0.0f;
                    for (int k = 0; k < Kuse; k++)
                        for (int c = 0; c < 3; c++)
                            s += g[n * 3 + c] * cp[n * K * 3 + k * 3 + c] * dbasis[n * Kuse * 3 + k * 3 + d];
                    dv[n * 3 + d] += s;
                }
            }
        }
    };
    return rgb;
}

} } // namespace mini::ops

#endif // MINI_OPS_SPHERICAL_HARMONICS_H
