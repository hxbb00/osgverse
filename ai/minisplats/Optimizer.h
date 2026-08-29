// mini/Optimizer.h — Adam optimizer + exponential LR scheduler.
//
// Replaces torch::optim::Adam and OptimScheduler.
// Supports the densification operations (add/remove parameter rows) that
// OpenSplat's afterTrain() requires: state tensors are grown/shrunk in lockstep
// with the parameter tensor.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_OPTIMIZER_H
#define MINI_OPTIMIZER_H

#include "Tensor.h"
#include <cmath>

namespace mini {

struct AdamOptions {
    float lr = 1e-3f;
    float beta1 = 0.9f;
    float beta2 = 0.999f;
    float eps = 1e-8f;
};

// Single-parameter Adam. The parameter is held by reference (caller owns it);
// the optimizer owns exp_avg / exp_avg_sq state buffers.
class Adam {
public:
    Tensor* param;
    AdamOptions opt;
    Tensor exp_avg;   // first moment
    Tensor exp_avg_sq; // second moment
    int64_t step_count = 0;

    Adam(Tensor* p, const AdamOptions& o) : param(p), opt(o) {}

    void ensure_state() {
        if (!exp_avg.defined() || exp_avg.numel() != param->numel()) {
            exp_avg = Tensor::zeros(param->sizes());
            exp_avg_sq = Tensor::zeros(param->sizes());
        }
    }

    void zero_grad() { if (param->defined()) param->zero_grad(); }

    void step() {
        ensure_state();
        Tensor g = param->grad();
        if (!g.defined()) return;
        step_count++;
        float bc1 = 1.0f - std::pow(opt.beta1, (float)step_count);
        float bc2 = 1.0f - std::pow(opt.beta2, (float)step_count);
        float* p = param->data_ptr();
        const float* gp = g.data_ptr();
        float* m = exp_avg.data_ptr();
        float* v = exp_avg_sq.data_ptr();
        for (int64_t i = 0; i < param->numel(); i++) {
            m[i] = opt.beta1 * m[i] + (1.0f - opt.beta1) * gp[i];
            v[i] = opt.beta2 * v[i] + (1.0f - opt.beta2) * gp[i] * gp[i];
            float m_hat = m[i] / bc1;
            float v_hat = v[i] / bc2;
            p[i] -= opt.lr * m_hat / (std::sqrt(v_hat) + opt.eps);
        }
    }

    void set_lr(float lr) { opt.lr = lr; }

    // --- Densification support (mirrors OpenSplat's addToOptimizer / removeFromOptimizer) ---

    // Grow state: append `n` zero rows for each index in `idcs` (splitting) or
    // 1 zero row per duplicated point.
    void add_rows(int64_t n_new_rows) {
        ensure_state();
        Shape old_s = param->sizes();
        Shape new_s = old_s;
        new_s[0] += n_new_rows;
        // Grow param
        Tensor new_param = Tensor::zeros(new_s);
        new_param.copy_block_from(0, *param, 0, param->numel());
        // Grow state
        Tensor new_m = Tensor::zeros(new_s);
        Tensor new_v = Tensor::zeros(new_s);
        new_m.copy_block_from(0, exp_avg, 0, exp_avg.numel());
        new_v.copy_block_from(0, exp_avg_sq, 0, exp_avg_sq.numel());
        // Swap into caller's tensor (param is a pointer; we overwrite the impl)
        *param = new_param;
        exp_avg = new_m;
        exp_avg_sq = new_v;
    }

    // Remove rows where mask[i] == 1 (1D boolean mask on first dim).
    void remove_rows(const std::vector<uint8_t>& mask) {
        ensure_state();
        Tensor kept = param->masked_select(mask);
        // For state, apply same mask on each row
        int64_t row_size = param->numel() / param->size(0);
        // Build masked versions of exp_avg / exp_avg_sq
        auto mask_state = [&](const Tensor& st) {
            Shape s = st.sizes();
            Shape out_s = s;
            int64_t count = 0;
            for (auto m : mask) if (!m) count++;
            out_s[0] = count;
            Tensor out = Tensor::zeros(out_s);
            int64_t dst = 0;
            for (int64_t i = 0; i < s[0]; i++) {
                if (!mask[i]) {
                    std::memcpy(&out.data_ptr()[dst * row_size],
                                &st.data_ptr()[i * row_size],
                                sizeof(float) * row_size);
                    dst++;
                }
            }
            return out;
        };
        *param = kept;
        exp_avg = mask_state(exp_avg);
        exp_avg_sq = mask_state(exp_avg_sq);
    }
};

// Exponential LR scheduler: lr = exp(log(lr_init)*(1-t) + log(lr_final)*t),
// where t = clamp(step / max_steps, 0, 1). Matches OpenSplat's OptimScheduler.
class ExponentialScheduler {
public:
    Adam* opt;
    float lr_init;
    float lr_final;
    int64_t max_steps;

    ExponentialScheduler(Adam* o, float lrI, float lrF, int64_t maxS)
        : opt(o), lr_init(lrI), lr_final(lrF), max_steps(maxS) {}

    float get_lr(int64_t step) const {
        float t = std::max(0.0f, std::min(static_cast<float>(step) / static_cast<float>(max_steps), 1.0f));
        return std::exp(std::log(lr_init) * (1.0f - t) + std::log(lr_final) * t);
    }

    void step(int64_t step) { opt->set_lr(get_lr(step)); }
};

} // namespace mini

#endif // MINI_OPTIMIZER_H
