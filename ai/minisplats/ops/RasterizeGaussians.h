// mini/ops/RasterizeGaussians.h — Tile-free alpha-blending rasterizer (forward + backward).
// Ported from rasterizer/gsplat-cpu/gsplat_cpu.cpp (rasterize_forward_tensor_cpu
// and rasterize_backward_tensor_cpu). The CPU algorithm sorts gaussians by
// depth and alpha-blends per pixel.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_OPS_RASTERIZE_GAUSSIANS_H
#define MINI_OPS_RASTERIZE_GAUSSIANS_H

#include "../Tensor.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace mini { namespace ops {

// Context stored for backward pass.
struct RasterCtx {
    int width, height;
    int numPoints;
    // Per-pixel list of contributing gaussian IDs (reversed for backward).
    std::shared_ptr<std::vector<int32_t>[]> px2gid;
    // final_Ts: [H*W] transmittance after last contributing gaussian
    std::vector<float> finalTs;
    // Inputs needed for backward
    std::vector<float> xys_data;      // [N*2]
    std::vector<float> conics_data;   // [N*3]
    std::vector<float> colors_data;   // [N*3]
    std::vector<float> opacities_data;// [N]
    std::vector<float> cov2d_data;    // [N*4] (2x2 row-major: xx, xy, yx, yy)
    std::vector<float> camDepths_data;// [N]
    float bgX, bgY, bgZ;
    // Input impls for grad accumulation
    std::shared_ptr<TensorImpl> xys_impl, conics_impl, colors_impl, opac_impl;
    bool xys_grad, conics_grad, colors_grad, opac_grad;
};

// Forward: rasterize gaussians to an RGB image.
//   xys:       [N, 2]  screen-space centers
//   conics:    [N, 3]  inverse 2D covariance (A, B, C)
//   colors:    [N, 3]  RGB
//   opacities: [N, 1]  alpha (already sigmoid'd)
//   background:[3]
//   cov2d:     [N, 4]  2x2 covariance (row-major: xx, xy, yx, yy)
//   camDepths: [N]
// Returns: rgb [H, W, 3]
inline Tensor rasterizeGaussians(
    int width, int height,
    const Tensor& xys, const Tensor& conics, const Tensor& colors,
    const Tensor& opacities, const Tensor& background,
    const Tensor& cov2d, const Tensor& camDepths)
{
    int numPoints = (int)xys.size(0);
    auto ctx = std::make_shared<RasterCtx>();
    ctx->width = width;
    ctx->height = height;
    ctx->numPoints = numPoints;
    ctx->px2gid = std::shared_ptr<std::vector<int32_t>[]>(new std::vector<int32_t>[width * height]);
    ctx->finalTs.assign(width * height, 1.0f);

    // Copy inputs (we need them for backward)
    ctx->xys_data.assign(xys.data_ptr(), xys.data_ptr() + numPoints * 2);
    ctx->conics_data.assign(conics.data_ptr(), conics.data_ptr() + numPoints * 3);
    ctx->colors_data.assign(colors.data_ptr(), colors.data_ptr() + numPoints * 3);
    ctx->opacities_data.assign(opacities.data_ptr(), opacities.data_ptr() + numPoints);
    ctx->cov2d_data.assign(cov2d.data_ptr(), cov2d.data_ptr() + numPoints * 4);
    ctx->camDepths_data.assign(camDepths.data_ptr(), camDepths.data_ptr() + numPoints);
    ctx->bgX = background.data_ptr()[0];
    ctx->bgY = background.data_ptr()[1];
    ctx->bgZ = background.data_ptr()[2];

    // Sort gaussian indices by depth (front to back)
    std::vector<size_t> gIndices(numPoints);
    std::iota(gIndices.begin(), gIndices.end(), 0);
    std::sort(gIndices.begin(), gIndices.end(), [&](int a, int b) {
        return ctx->camDepths_data[a] < ctx->camDepths_data[b];
    });

    // Output image
    auto rgb = Tensor::zeros({height, width, 3});
    float* pOut = rgb.data_ptr();
    std::vector<uint8_t> done(width * height, 0);

    const float alphaThresh = 1.0f / 255.0f;

    for (int idx = 0; idx < numPoints; idx++) {
        int32_t gid = (int32_t)gIndices[idx];
        float A = ctx->conics_data[gid * 3 + 0];
        float B = ctx->conics_data[gid * 3 + 1];
        float C = ctx->conics_data[gid * 3 + 2];
        float gX = ctx->xys_data[gid * 2 + 0];
        float gY = ctx->xys_data[gid * 2 + 1];
        float sqx = 3.0f * std::sqrt(ctx->cov2d_data[gid * 4 + 0]); // [0,0]
        float sqy = 3.0f * std::sqrt(ctx->cov2d_data[gid * 4 + 3]); // [1,1]

        int minx = (std::max)(0, (int)std::floor(gY - sqy) - 2);
        int maxx = (std::min)(height, (int)std::ceil(gY + sqy) + 2);
        int miny = (std::max)(0, (int)std::floor(gX - sqx) - 2);
        int maxy = (std::min)(width, (int)std::ceil(gX + sqx) + 2);

        for (int i = minx; i < maxx; i++) {
            for (int j = miny; j < maxy; j++) {
                size_t pixIdx = (size_t)i * width + j;
                if (done[pixIdx]) continue;
                float xCam = gX - j;
                float yCam = gY - i;
                float sigma = 0.5f * (A * xCam * xCam + C * yCam * yCam) + B * xCam * yCam;
                if (sigma < 0.0f) continue;
                float alpha = (std::min)(0.999f, ctx->opacities_data[gid] * std::exp(-sigma));
                if (alpha < alphaThresh) continue;
                float T = ctx->finalTs[pixIdx];
                float nextT = T * (1.0f - alpha);
                if (nextT <= 1e-4f) { done[pixIdx] = 1; continue; }
                float vis = alpha * T;
                pOut[pixIdx * 3 + 0] += vis * ctx->colors_data[gid * 3 + 0];
                pOut[pixIdx * 3 + 1] += vis * ctx->colors_data[gid * 3 + 1];
                pOut[pixIdx * 3 + 2] += vis * ctx->colors_data[gid * 3 + 2];
                ctx->finalTs[pixIdx] = nextT;
                ctx->px2gid[pixIdx].push_back(gid);
            }
        }
    }

    // Background fill + reverse per-pixel gaussian lists for backward
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            size_t pixIdx = (size_t)i * width + j;
            float T = ctx->finalTs[pixIdx];
            pOut[pixIdx * 3 + 0] += T * ctx->bgX;
            pOut[pixIdx * 3 + 1] += T * ctx->bgY;
            pOut[pixIdx * 3 + 2] += T * ctx->bgZ;
            std::reverse(ctx->px2gid[pixIdx].begin(), ctx->px2gid[pixIdx].end());
        }
    }

    // Autograd setup
    ctx->xys_impl = xys.impl();   ctx->xys_grad = xys.requires_grad() && !is_no_grad();
    ctx->conics_impl = conics.impl(); ctx->conics_grad = conics.requires_grad() && !is_no_grad();
    ctx->colors_impl = colors.impl(); ctx->colors_grad = colors.requires_grad() && !is_no_grad();
    ctx->opac_impl = opacities.impl(); ctx->opac_grad = opacities.requires_grad() && !is_no_grad();

    bool any_grad = ctx->xys_grad || ctx->conics_grad || ctx->colors_grad || ctx->opac_grad;
    if (any_grad) {
        rgb.impl()->requires_grad = true;
        if (ctx->xys_grad) rgb.impl()->inputs.push_back(xys.impl());
        if (ctx->conics_grad) rgb.impl()->inputs.push_back(conics.impl());
        if (ctx->colors_grad) rgb.impl()->inputs.push_back(colors.impl());
        if (ctx->opac_grad) rgb.impl()->inputs.push_back(opacities.impl());

        rgb.impl()->backward_fn = [ctx](TensorImpl& self) -> void {
            const float* pv_output = self.grad->data.data();
            const int H = ctx->height, W = ctx->width;
            const int N = ctx->numPoints;
            // Allocate grad buffers
            std::vector<float> v_xy(N * 2, 0.0f), v_conic(N * 3, 0.0f);
            std::vector<float> v_colors(N * 3, 0.0f), v_opacity(N, 0.0f);

            const float alphaThresh = 1.0f / 255.0f;
            for (int i = 0; i < H; i++) {
                for (int j = 0; j < W; j++) {
                    size_t pixIdx = (size_t)i * W + j;
                    float Tfinal = ctx->finalTs[pixIdx];
                    float T = Tfinal;
                    float buffer[3] = {0, 0, 0};
                    for (const int32_t& gid : ctx->px2gid[pixIdx]) {
                        float A = ctx->conics_data[gid * 3 + 0];
                        float B = ctx->conics_data[gid * 3 + 1];
                        float C = ctx->conics_data[gid * 3 + 2];
                        float gX = ctx->xys_data[gid * 2 + 0];
                        float gY = ctx->xys_data[gid * 2 + 1];
                        float xCam = gX - j;
                        float yCam = gY - i;
                        float sigma = 0.5f * (A * xCam * xCam + C * yCam * yCam) + B * xCam * yCam;
                        if (sigma < 0.0f) continue;
                        float vis = std::exp(-sigma);
                        float alpha = (std::min)(0.99f, ctx->opacities_data[gid] * vis);
                        if (alpha < alphaThresh) continue;
                        float ra = 1.0f / (1.0f - alpha);
                        T *= ra;
                        float fac = alpha * T;
                        v_colors[gid * 3 + 0] += fac * pv_output[pixIdx * 3 + 0];
                        v_colors[gid * 3 + 1] += fac * pv_output[pixIdx * 3 + 1];
                        v_colors[gid * 3 + 2] += fac * pv_output[pixIdx * 3 + 2];
                        float v_alpha = ((ctx->colors_data[gid * 3 + 0] * T - buffer[0] * ra) * pv_output[pixIdx * 3 + 0]) +
                                        ((ctx->colors_data[gid * 3 + 1] * T - buffer[1] * ra) * pv_output[pixIdx * 3 + 1]) +
                                        ((ctx->colors_data[gid * 3 + 2] * T - buffer[2] * ra) * pv_output[pixIdx * 3 + 2]) +
                                        (-Tfinal * ra * ctx->bgX * pv_output[pixIdx * 3 + 0]) +
                                        (-Tfinal * ra * ctx->bgY * pv_output[pixIdx * 3 + 1]) +
                                        (-Tfinal * ra * ctx->bgZ * pv_output[pixIdx * 3 + 2]);
                        buffer[0] += ctx->colors_data[gid * 3 + 0] * fac;
                        buffer[1] += ctx->colors_data[gid * 3 + 1] * fac;
                        buffer[2] += ctx->colors_data[gid * 3 + 2] * fac;
                        float v_sigma = -ctx->opacities_data[gid] * vis * v_alpha;
                        v_conic[gid * 3 + 0] += 0.5f * v_sigma * xCam * xCam;
                        v_conic[gid * 3 + 1] += 0.5f * v_sigma * xCam * yCam;
                        v_conic[gid * 3 + 2] += 0.5f * v_sigma * yCam * yCam;
                        v_xy[gid * 2 + 0] += v_sigma * (A * xCam + B * yCam);
                        v_xy[gid * 2 + 1] += v_sigma * (B * xCam + C * yCam);
                        v_opacity[gid] += vis * v_alpha;
                    }
                }
            }
            // Accumulate into input grads
            if (ctx->xys_grad) {
                ctx->xys_impl->ensure_grad();
                for (int k = 0; k < N * 2; k++) ctx->xys_impl->grad->data[k] += v_xy[k];
            }
            if (ctx->conics_grad) {
                ctx->conics_impl->ensure_grad();
                for (int k = 0; k < N * 3; k++) ctx->conics_impl->grad->data[k] += v_conic[k];
            }
            if (ctx->colors_grad) {
                ctx->colors_impl->ensure_grad();
                for (int k = 0; k < N * 3; k++) ctx->colors_impl->grad->data[k] += v_colors[k];
            }
            if (ctx->opac_grad) {
                ctx->opac_impl->ensure_grad();
                for (int k = 0; k < N; k++) ctx->opac_impl->grad->data[k] += v_opacity[k];
            }
        };
    }
    return rgb;
}

} } // namespace mini::ops

#endif // MINI_OPS_RASTERIZE_GAUSSIANS_H
