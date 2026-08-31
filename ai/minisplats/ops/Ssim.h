// mini/ops/Ssim.h — SSIM loss with autograd.
// Ported from ssim.cpp (pytorch-ssim, MIT). Replaces torch::nn::functional::conv2d
// with a custom depthwise conv2d op (forward + backward) built on the mini Tensor.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_OPS_SSIM_H
#define MINI_OPS_SSIM_H

#include "../Tensor.h"
#include <cmath>
#include <vector>

namespace mini { namespace ops {

// ---- Depthwise 2D convolution (custom autograd op) ----
//   input:  [H, W, C]
//   window: [K, K, C]  (one kernel per channel)
//   output: [H, W, C]  (same size, zero-padded by K/2)
//   out[h,w,c] = sum_{i,j} window[i,j,c] * input[h+i-K/2, w+j-K/2, c]
//
// Backward: since the Gaussian window is symmetric, transposed conv = conv.
// We implement it as correlation with the (symmetric) window, which is correct
// for symmetric windows and a standard approximation otherwise.
inline Tensor depthwiseConv2D(const Tensor& input, const Tensor& window) {
    int H = (int)input.size(0);
    int W = (int)input.size(1);
    int C = (int)input.size(2);
    int K = (int)window.size(0);
    int half = K / 2;

    auto output = Tensor::zeros({H, W, C});
    const float* in = input.data_ptr();
    const float* win = window.data_ptr();
    float* out = output.data_ptr();

    for (int c = 0; c < C; c++) {
        for (int h = 0; h < H; h++) {
            for (int w = 0; w < W; w++) {
                float s = 0.0f;
                for (int i = 0; i < K; i++) {
                    int ih = h + i - half;
                    if (ih < 0 || ih >= H) continue;
                    for (int j = 0; j < K; j++) {
                        int jw = w + j - half;
                        if (jw < 0 || jw >= W) continue;
                        s += win[i * K * C + j * C + c] * in[ih * W * C + jw * C + c];
                    }
                }
                out[h * W * C + w * C + c] = s;
            }
        }
    }

    // Autograd setup
    if (input.requires_grad() && !is_no_grad()) {
        output.impl()->requires_grad = true;
        output.impl()->inputs.push_back(input.impl());
        auto in_impl = input.impl();
        // Capture window by value (shared_ptr)
        auto win_impl = window.impl();
        output.impl()->backward_fn = [in_impl, win_impl, H, W, C, K, half](TensorImpl& self) -> void {
            if (!in_impl->requires_grad) return;
            in_impl->ensure_grad();
            const float* g = self.grad->data.data();
            const float* win = win_impl->data.data();
            float* din = in_impl->grad->data.data();
            // Transposed depthwise conv: grad_input = conv(grad_output, window)
            // (valid for symmetric windows; Gaussian is symmetric)
            for (int c = 0; c < C; c++) {
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        float s = 0.0f;
                        for (int i = 0; i < K; i++) {
                            int ih = h + i - half;
                            if (ih < 0 || ih >= H) continue;
                            for (int j = 0; j < K; j++) {
                                int jw = w + j - half;
                                if (jw < 0 || jw >= W) continue;
                                s += win[i * K * C + j * C + c] * g[ih * W * C + jw * C + c];
                            }
                        }
                        din[h * W * C + w * C + c] += s;
                    }
                }
            }
        };
    }
    return output;
}

// ---- Gaussian window (11x11, sigma=1.5) ----
// Returns [K, K, C] tensor. Matches ssim.cpp createWindow/gaussian.
inline Tensor createGaussianWindow(int windowSize = 11, int channels = 3,
                                    float sigma = 1.5f) {
    // 1D Gaussian
    std::vector<float> w1d(windowSize);
    float sum = 0.0f;
    for (int i = 0; i < windowSize; i++) {
        float x = std::floor((float)(i - windowSize) / 2.0f);
        w1d[i] = std::exp(-(x * x) / (2.0f * sigma * sigma));
        sum += w1d[i];
    }
    for (int i = 0; i < windowSize; i++) w1d[i] /= sum;

    // 2D = outer product, expanded per channel
    auto window = Tensor::zeros({windowSize, windowSize, channels});
    float* w = window.data_ptr();
    for (int c = 0; c < channels; c++)
        for (int i = 0; i < windowSize; i++)
            for (int j = 0; j < windowSize; j++)
                w[i * windowSize * channels + j * channels + c] = w1d[i] * w1d[j];
    return window;
}

// ---- SSIM metric (returns mean SSIM, higher = better) ----
//   rendered: [H, W, 3]
//   gt:       [H, W, 3]
//   window:   [K, K, 3]  (from createGaussianWindow)
// Returns: scalar tensor (mean SSIM)
inline Tensor ssim(const Tensor& rendered, const Tensor& gt, const Tensor& window) {
    const float C1 = 0.01f * 0.01f;
    const float C2 = 0.03f * 0.03f;

    // mu1 = conv(gt, w), mu2 = conv(rendered, w)
    // Note: gt typically has requires_grad=false; rendered has requires_grad=true.
    Tensor mu1 = depthwiseConv2D(gt, window);
    Tensor mu2 = depthwiseConv2D(rendered, window);

    Tensor mu1Sq = mu1 * mu1;
    Tensor mu2Sq = mu2 * mu2;
    Tensor mu1mu2 = mu1 * mu2;

    Tensor sigma1Sq = depthwiseConv2D(gt * gt, window) - mu1Sq;
    Tensor sigma2Sq = depthwiseConv2D(rendered * rendered, window) - mu2Sq;
    Tensor sigma12 = depthwiseConv2D(gt * rendered, window) - mu1mu2;

    Tensor ssimMap =
        ((2.0f * mu1mu2 + C1) * (2.0f * sigma12 + C2)) /
        ((mu1Sq + mu2Sq + C1) * (sigma1Sq + sigma2Sq + C2));

    return ssimMap.mean();
}

// ---- SSIM loss (1 - SSIM, lower = better) ----
inline Tensor ssimLoss(const Tensor& rendered, const Tensor& gt, const Tensor& window) {
    return Tensor::scalar(1.0f) - ssim(rendered, gt, window);
}

} } // namespace mini::ops

#endif // MINI_OPS_SSIM_H
