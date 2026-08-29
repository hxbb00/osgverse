// mini/Tensor.h — Minimal float32 tensor library with reverse-mode autograd.
// Designed to replace libtorch for CPU-only OpenSplat.
//
// Design:
//   - Tensor is a handle (shared_ptr<TensorImpl>), cheap to copy.
//   - Row-major contiguous storage, float32 only.
//   - Autograd: ops on requires_grad tensors build a DAG; backward() walks it
//     in reverse topological order, accumulating into .grad of leaves.
//   - No broadcasting between tensors of different shapes; use scalar ops or
//     explicit expand/repeat. (Keeps the implementation small.)
//
// SPDX-License-Identifier: MIT
#ifndef MINI_TENSOR_H
#define MINI_TENSOR_H

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace mini {

using Shape = std::vector<int64_t>;

inline int64_t numel(const Shape& s) {
    return std::accumulate(s.begin(), s.end(), int64_t{1}, std::multiplies<int64_t>{});
}

inline size_t total(const Shape& s) { return static_cast<size_t>(numel(s)); }

// ---- TensorImpl: shared state -------------------------------------------------
struct TensorImpl {
    std::vector<float> data;
    Shape shape;

    // Autograd
    bool requires_grad = false;
    bool retain_grad = false;
    std::shared_ptr<TensorImpl> grad;          // gradient (null if none yet)
    std::function<void(TensorImpl&)> backward_fn; // accumulates into inputs' grads
    std::vector<std::shared_ptr<TensorImpl>> inputs; // graph children (keeps alive)

    TensorImpl() = default;
    explicit TensorImpl(Shape s) : data(total(s), 0.0f), shape(std::move(s)) {}
    TensorImpl(Shape s, float fill) : data(total(s), fill), shape(std::move(s)) {}
    TensorImpl(std::vector<float> d, Shape s) : data(std::move(d)), shape(std::move(s)) {}

    void ensure_grad() {
        if (!grad) grad = std::make_shared<TensorImpl>(shape, 0.0f);
        if (grad->data.size() != data.size())
            grad->data.assign(data.size(), 0.0f);
    }
};

// ---- NoGradGuard --------------------------------------------------------------
struct NoGradGuard {
    static bool& flag() { static bool f = false; return f; }
    NoGradGuard()  { flag() = true; }
    ~NoGradGuard() { flag() = false; }
};
inline bool is_no_grad() { return NoGradGuard::flag(); }

// ---- Tensor handle ------------------------------------------------------------
class Tensor {
public:
    Tensor() = default;
    explicit Tensor(std::shared_ptr<TensorImpl> p) : impl_(std::move(p)) {}

    // ---- Factories ----
    static Tensor zeros(const Shape& s) {
        return Tensor(std::make_shared<TensorImpl>(s, 0.0f));
    }
    static Tensor ones(const Shape& s) {
        return Tensor(std::make_shared<TensorImpl>(s, 1.0f));
    }
    static Tensor full(const Shape& s, float v) {
        return Tensor(std::make_shared<TensorImpl>(s, v));
    }
    static Tensor empty(const Shape& s) {
        auto p = std::make_shared<TensorImpl>(s);
        return Tensor(p);
    }
    static Tensor scalar(float v) {
        return Tensor(std::make_shared<TensorImpl>(std::vector<float>{v}, Shape{}));
    }
    static Tensor from_blob(const float* ptr, const Shape& s) {
        auto p = std::make_shared<TensorImpl>(std::vector<float>(ptr, ptr + total(s)), s);
        return Tensor(p);
    }
    static Tensor arange(float start, float stop, float step = 1.0f) {
        std::vector<float> d;
        for (float v = start; v < stop; v += step) d.push_back(v);
        int64_t n = (int64_t)d.size();
        return Tensor(std::make_shared<TensorImpl>(std::move(d), Shape{n}));
    }
    static Tensor eye(int64_t n) {
        auto t = zeros({n, n});
        for (int64_t i = 0; i < n; i++) t.impl_->data[i * n + i] = 1.0f;
        return t;
    }
    static Tensor randn(const Shape& s, uint32_t seed = 0) {
        static std::mt19937 rng(seed ? seed : std::random_device{}());
        std::normal_distribution<float> dist(0.0f, 1.0f);
        auto p = std::make_shared<TensorImpl>(s);
        for (auto& v : p->data) v = dist(rng);
        return Tensor(p);
    }
    static Tensor uniform(const Shape& s, float lo = 0.0f, float hi = 1.0f, uint32_t seed = 0) {
        static std::mt19937 rng(seed ? seed : std::random_device{}());
        std::uniform_real_distribution<float> dist(lo, hi);
        auto p = std::make_shared<TensorImpl>(s);
        for (auto& v : p->data) v = dist(rng);
        return Tensor(p);
    }

    // ---- Shape queries ----
    const Shape& sizes() const { return impl_->shape; }
    int64_t size(int64_t dim) const {
        if (dim < 0) dim += impl_->shape.size();
        return impl_->shape[dim];
    }
    int64_t numel() const { return (int64_t)impl_->data.size(); }
    int64_t dim() const { return (int64_t)impl_->shape.size(); }
    int64_t ndim() const { return dim(); }
    bool defined() const { return impl_ != nullptr; }

    // ---- Data access ----
    float* data_ptr() { return impl_->data.data(); }
    const float* data_ptr() const { return impl_->data.data(); }
    float item() const {
        assert(numel() == 1);
        return impl_->data[0];
    }
    float& at(const std::vector<int64_t>& idx) {
        int64_t off = 0;
        for (size_t i = 0; i < idx.size(); i++) off = off * impl_->shape[i] + idx[i];
        return impl_->data[off];
    }
    float at(const std::vector<int64_t>& idx) const {
        int64_t off = 0;
        for (size_t i = 0; i < idx.size(); i++) off = off * impl_->shape[i] + idx[i];
        return impl_->data[off];
    }

    // ---- Autograd ----
    bool requires_grad() const { return impl_->requires_grad; }
    Tensor& requires_grad_(bool v = true) { impl_->requires_grad = v; return *this; }
    Tensor& retain_grad() { impl_->retain_grad = true; return *this; }
    Tensor grad() const {
        return impl_->grad ? Tensor(impl_->grad) : Tensor();
    }
    Tensor detach() const {
        auto p = std::make_shared<TensorImpl>(impl_->data, impl_->shape);
        return Tensor(p);
    }
    Tensor contiguous() const { return detach(); }

    // Manual grad zeroing
    void zero_grad() {
        if (impl_->grad) std::fill(impl_->grad->data.begin(), impl_->grad->data.end(), 0.0f);
    }

    // Backward: topological sort then call backward_fn in reverse order.
    void backward(const Tensor& seed = Tensor()) {
        if (!impl_->requires_grad) return;
        // Seed
        impl_->ensure_grad();
        if (seed.defined()) {
            assert(seed.numel() == numel());
            for (size_t i = 0; i < impl_->data.size(); i++)
                impl_->grad->data[i] += seed.impl_->data[i];
        } else {
            impl_->grad->data[0] += 1.0f; // scalar output default seed
        }

        // Topo sort via DFS
        std::vector<TensorImpl*> order;
        std::unordered_set<TensorImpl*> visited;
        std::function<void(TensorImpl*)> dfs = [&](TensorImpl* n) {
            if (!n || visited.count(n)) return;
            visited.insert(n);
            for (auto& in : n->inputs) dfs(in.get());
            order.push_back(n);
        };
        dfs(impl_.get());

        // Reverse topo: call backward_fn
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            TensorImpl* n = *it;
            if (!n->backward_fn || !n->grad) continue;
            n->backward_fn(*n);
        }
    }

    // ---- Shape ops (non-grad; create new contiguous buffers) ----
    Tensor reshape(const Shape& s) const {
        auto p = std::make_shared<TensorImpl>(impl_->data, s);
        return Tensor(p);
    }
    Tensor view(const Shape& s) const { return reshape(s); }
    Tensor flatten() const {
        Shape s{numel()};
        return reshape(s);
    }
    Tensor unsqueeze(int64_t dim) const {
        Shape s = impl_->shape;
        if (dim < 0) dim += s.size() + 1;
        s.insert(s.begin() + dim, 1);
        return reshape(s);
    }
    Tensor squeeze(int64_t dim = -1) const {
        Shape s = impl_->shape;
        if (dim < 0) {
            // squeeze all size-1 dims
            Shape out;
            for (auto d : s) if (d != 1) out.push_back(d);
            if (out.empty()) out.push_back(1);
            return reshape(out);
        }
        s.erase(s.begin() + dim);
        return reshape(s);
    }
    Tensor transpose(int64_t a = 0, int64_t b = 1) const {
        assert(impl_->shape.size() == 2);
        int64_t r = impl_->shape[0], c = impl_->shape[1];
        auto p = std::make_shared<TensorImpl>(Shape{c, r});
        for (int64_t i = 0; i < r; i++)
            for (int64_t j = 0; j < c; j++)
                p->data[j * r + i] = impl_->data[i * c + j];
        return Tensor(p);
    }
    Tensor t() const { return transpose(); }

    // Slice along dim: [start, end)
    Tensor narrow(int64_t dim, int64_t start, int64_t len) const {
        if (dim < 0) dim += impl_->shape.size();
        Shape out_shape = impl_->shape;
        out_shape[dim] = len;
        auto p = std::make_shared<TensorImpl>(out_shape);
        // Compute strides for source
        Shape strides(impl_->shape.size(), 1);
        for (int i = (int)impl_->shape.size() - 2; i >= 0; i--)
            strides[i] = strides[i + 1] * impl_->shape[i + 1];
        int64_t out_num = total(out_shape);
        Shape out_strides = strides;
        out_strides[dim] = 0; // handled separately
        // General copy
        std::vector<int64_t> idx(impl_->shape.size(), 0);
        for (int64_t o = 0; o < out_num; o++) {
            // Decode output linear -> multi-index
            int64_t rem = o;
            for (int k = 0; k < (int)out_shape.size(); k++) {
                idx[k] = rem / (k + 1 < (int)out_shape.size() ? [&]{
                    int64_t s = 1;
                    for (int m = k + 1; m < (int)out_shape.size(); m++) s *= out_shape[m];
                    return s;
                }() : 1);
                rem = o;
                for (int k2 = 0; k2 <= k; k2++) {
                    int64_t s = 1;
                    for (int m = k2 + 1; m < (int)out_shape.size(); m++) s *= out_shape[m];
                    rem -= idx[k2] * s;
                }
            }
            // Source offset
            int64_t src_off = start * strides[dim];
            for (int k = 0; k < (int)idx.size(); k++)
                if (k != dim) src_off += idx[k] * strides[k];
            p->data[o] = impl_->data[src_off];
        }
        return Tensor(p);
    }

    // Simple 2D slice: rows [r0,r1), cols [c0,c1) — most common pattern.
    Tensor slice2d(int64_t r0, int64_t r1, int64_t c0, int64_t c1) const {
        assert(impl_->shape.size() == 2);
        int64_t cols = impl_->shape[1];
        Shape out{r1 - r0, c1 - c0};
        auto p = std::make_shared<TensorImpl>(out);
        for (int64_t i = 0; i < out[0]; i++)
            std::memcpy(&p->data[i * out[1]],
                        &impl_->data[(r0 + i) * cols + c0],
                        sizeof(float) * out[1]);
        return Tensor(p);
    }

    // Select single index along dim (drops the dim).
    Tensor select(int64_t dim, int64_t idx) const {
        if (dim < 0) dim += impl_->shape.size();
        Shape out_shape;
        for (int i = 0; i < (int)impl_->shape.size(); i++)
            if (i != dim) out_shape.push_back(impl_->shape[i]);
        if (out_shape.empty()) out_shape.push_back(1);
        auto p = std::make_shared<TensorImpl>(out_shape);
        // strides
        Shape strides(impl_->shape.size(), 1);
        for (int i = (int)impl_->shape.size() - 2; i >= 0; i--)
            strides[i] = strides[i + 1] * impl_->shape[i + 1];
        int64_t src_base = idx * strides[dim];
        int64_t out_num = total(out_shape);
        // Iterate over all non-dim indices
        std::vector<int64_t> other_dims;
        for (int i = 0; i < (int)impl_->shape.size(); i++)
            if (i != dim) other_dims.push_back(i);
        Shape other_shape;
        for (int d : other_dims) other_shape.push_back(impl_->shape[d]);
        for (int64_t o = 0; o < out_num; o++) {
            int64_t src_off = src_base;
            int64_t rem = o;
            for (int k = (int)other_dims.size() - 1; k >= 0; k--) {
                int64_t d = other_dims[k];
                int64_t coord = rem % impl_->shape[d];
                rem /= impl_->shape[d];
                src_off += coord * strides[d];
            }
            p->data[o] = impl_->data[src_off];
        }
        return Tensor(p);
    }

    // Repeat each element along a dim n times (inserts new dim of size n before dim).
    // Matches torch::repeat_interleave semantics for 1D-ish use in densify.
    Tensor repeat_interleave(int64_t dim, int64_t n) const {
        if (dim < 0) dim += impl_->shape.size();
        Shape out_shape = impl_->shape;
        out_shape.insert(out_shape.begin() + dim, n);
        auto p = std::make_shared<TensorImpl>(out_shape);
        // strides of source
        Shape strides(impl_->shape.size(), 1);
        for (int i = (int)impl_->shape.size() - 2; i >= 0; i--)
            strides[i] = strides[i + 1] * impl_->shape[i + 1];
        int64_t src_num = total(impl_->shape);
        int64_t block = total(impl_->shape) ;
        // Compute block sizes: product of dims after `dim`
        int64_t tail = 1;
        for (size_t i = dim + 1; i < impl_->shape.size(); i++) tail *= impl_->shape[i];
        int64_t head = 1;
        for (int64_t i = 0; i < dim; i++) head *= impl_->shape[i];
        // Output strides
        Shape out_strides(out_shape.size(), 1);
        for (int i = (int)out_shape.size() - 2; i >= 0; i--)
            out_strides[i] = out_strides[i + 1] * out_shape[i + 1];
        for (int64_t h = 0; h < head; h++) {
            for (int64_t r = 0; r < n; r++) {
                for (int64_t t = 0; t < tail; t++) {
                    int64_t out_off = h * out_strides[dim - (dim > 0 ? 0 : 0)] * 0; // placeholder
                    // Simpler: direct offset computation
                    int64_t o = (h * n + r) * tail + t;
                    int64_t s = h * tail + t;
                    p->data[o] = impl_->data[s];
                }
            }
        }
        return Tensor(p);
    }

    // Tile whole tensor n times along a NEW leading dim.
    Tensor repeat_lead(int64_t n) const {
        Shape s{ n };
        for (auto d : impl_->shape) s.push_back(d);
        auto p = std::make_shared<TensorImpl>(s);
        int64_t one = numel();
        for (int64_t i = 0; i < n; i++)
            std::memcpy(&p->data[i * one], impl_->data.data(), sizeof(float) * one);
        return Tensor(p);
    }

    // ---- Boolean mask indexing (1D mask on first dim) ----
    // Returns rows where mask[i] != 0.
    Tensor masked_select(const std::vector<uint8_t>& mask) const {
        assert((int64_t)mask.size() == impl_->shape[0]);
        int64_t row_size = 1;
        for (size_t i = 1; i < impl_->shape.size(); i++) row_size *= impl_->shape[i];
        int64_t count = 0;
        for (auto m : mask) if (m) count++;
        Shape out_shape = impl_->shape;
        out_shape[0] = count;
        auto p = std::make_shared<TensorImpl>(out_shape);
        int64_t dst = 0;
        for (int64_t i = 0; i < impl_->shape[0]; i++) {
            if (mask[i]) {
                std::memcpy(&p->data[dst * row_size],
                            &impl_->data[i * row_size],
                            sizeof(float) * row_size);
                dst++;
            }
        }
        return Tensor(p);
    }

    // Return indices where mask is true (1D).
    std::vector<int64_t> nonzero_mask(const std::vector<uint8_t>& mask) const {
        std::vector<int64_t> out;
        for (size_t i = 0; i < mask.size(); i++) if (mask[i]) out.push_back(i);
        return out;
    }

    // ---- Concatenation (along first dim) ----
    static Tensor cat(const std::vector<Tensor>& ts, int64_t dim = 0) {
        assert(!ts.empty());
        if (dim < 0) dim += ts[0].impl_->shape.size();
        Shape out_shape = ts[0].impl_->shape;
        out_shape[dim] = 0;
        for (auto& t : ts) out_shape[dim] += t.impl_->shape[dim];
        auto p = std::make_shared<TensorImpl>(out_shape);
        // strides
        Shape strides(out_shape.size(), 1);
        for (int i = (int)out_shape.size() - 2; i >= 0; i--)
            strides[i] = strides[i + 1] * out_shape[i + 1];
        int64_t tail = 1;
        for (int i = dim + 1; i < (int)out_shape.size(); i++) tail *= out_shape[i];
        int64_t offset = 0;
        for (auto& t : ts) {
            int64_t rows = t.impl_->shape[dim];
            int64_t tail_src = 1;
            for (int i = dim + 1; i < (int)t.impl_->shape.size(); i++) tail_src *= t.impl_->shape[i];
            assert(tail_src == tail);
            int64_t head_src = 1;
            for (int i = 0; i < dim; i++) head_src *= t.impl_->shape[i];
            for (int64_t h = 0; h < head_src; h++) {
                std::memcpy(&p->data[(h * out_shape[dim] + offset) * tail + 0 * 0],
                            &t.impl_->data[h * rows * tail],
                            sizeof(float) * rows * tail);
            }
            offset += rows;
        }
        // Autograd
        bool any_grad = false;
        for (auto& t : ts) if (t.impl_->requires_grad) { any_grad = true; break; }
        if (any_grad && !is_no_grad()) {
            p->requires_grad = true;
            for (auto& t : ts) p->inputs.push_back(t.impl_);
            std::vector<Shape> shapes;
            for (auto& t : ts) shapes.push_back(t.impl_->shape);
            p->backward_fn = [shapes, dim, out_shape](TensorImpl& self) -> void {
                int64_t tail = 1;
                for (int i = dim + 1; i < (int)out_shape.size(); i++) tail *= out_shape[i];
                int64_t offset = 0;
                for (size_t k = 0; k < self.inputs.size(); k++) {
                    auto& in = self.inputs[k];
                    if (!in->requires_grad) { offset += shapes[k][dim]; continue; }
                    in->ensure_grad();
                    int64_t rows = shapes[k][dim];
                    int64_t head = 1;
                    for (int i = 0; i < dim; i++) head *= shapes[k][i];
                    for (int64_t h = 0; h < head; h++) {
                        for (int64_t r = 0; r < rows; r++) {
                            int64_t src = (h * out_shape[dim] + offset + r) * tail;
                            int64_t dst = (h * rows + r) * tail;
                            for (int64_t t = 0; t < tail; t++)
                                in->grad->data[dst + t] += self.grad->data[src + t];
                        }
                    }
                    offset += rows;
                }
            };
        }
        return Tensor(p);
    }

    static Tensor stack(const std::vector<Tensor>& ts, int64_t dim = 0) {
        assert(!ts.empty());
        Shape out_shape = ts[0].impl_->shape;
        out_shape.insert(out_shape.begin() + dim, ts.size());
        auto p = std::make_shared<TensorImpl>(out_shape);
        int64_t one = ts[0].numel();
        for (size_t i = 0; i < ts.size(); i++)
            std::memcpy(&p->data[i * one], ts[i].impl_->data.data(), sizeof(float) * one);
        return Tensor(p);
    }

    // ---- Element-wise binary ops (with autograd) ----
    Tensor operator+(const Tensor& o) const { return binary(o, 0); }
    Tensor operator-(const Tensor& o) const { return binary(o, 1); }
    Tensor operator*(const Tensor& o) const { return binary(o, 2); }
    Tensor operator/(const Tensor& o) const { return binary(o, 3); }

    Tensor operator+(float v) const { return *this + Tensor::full(impl_->shape, v); }
    Tensor operator-(float v) const { return *this - Tensor::full(impl_->shape, v); }
    Tensor operator*(float v) const { return *this * Tensor::full(impl_->shape, v); }
    Tensor operator/(float v) const { return *this / Tensor::full(impl_->shape, v); }

    Tensor binary(const Tensor& o, int op) const {
        assert(impl_->shape == o.impl_->shape);
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++) {
            float a = impl_->data[i], b = o.impl_->data[i];
            p->data[i] = op == 0 ? a + b : op == 1 ? a - b : op == 2 ? a * b : a / b;
        }
        setup_binary_backward(p, *this, o, op);
        return Tensor(p);
    }

    void setup_binary_backward(std::shared_ptr<TensorImpl>& p,
                               const Tensor& a, const Tensor& b, int op) const {
        bool a_grad = a.impl_->requires_grad && !is_no_grad();
        bool b_grad = b.impl_->requires_grad && !is_no_grad();
        if (!a_grad && !b_grad) return;
        p->requires_grad = true;
        if (a_grad) p->inputs.push_back(a.impl_);
        if (b_grad) p->inputs.push_back(b.impl_);
        p->backward_fn = [a_impl = a.impl_, b_impl = b.impl_, op, a_grad, b_grad](TensorImpl& self) -> void {
            const auto& g = self.grad->data;
            if (a_grad) {
                a_impl->ensure_grad();
                auto& ag = a_impl->grad->data;
                for (size_t i = 0; i < g.size(); i++) {
                    if (op == 0) ag[i] += g[i];              // a+b
                    else if (op == 1) ag[i] += g[i];         // a-b
                    else if (op == 2) ag[i] += g[i] * b_impl->data[i]; // a*b
                    else ag[i] += g[i] / b_impl->data[i];    // a/b
                }
            }
            if (b_grad) {
                b_impl->ensure_grad();
                auto& bg = b_impl->grad->data;
                for (size_t i = 0; i < g.size(); i++) {
                    if (op == 0) bg[i] += g[i];                       // a+b
                    else if (op == 1) bg[i] -= g[i];                  // a-b
                    else if (op == 2) bg[i] += g[i] * a_impl->data[i]; // a*b
                    else bg[i] -= g[i] * a_impl->data[i] / (b_impl->data[i] * b_impl->data[i]); // a/b
                }
            }
        };
    }

    // ---- Unary element-wise (with autograd) ----
    Tensor unary(float (*f)(float), float (*df)(float, float)) const {
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++)
            p->data[i] = f(impl_->data[i]);
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            p->backward_fn = [src, df](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                for (size_t i = 0; i < self.data.size(); i++)
                    src->grad->data[i] += self.grad->data[i] * df(src->data[i], self.data[i]);
            };
        }
        return Tensor(p);
    }

    Tensor exp() const  { return unary([](float x){return std::exp(x);},  [](float x, float){return std::exp(x);}); }
    Tensor log() const  { return unary([](float x){return std::log(x);},  [](float x, float){return 1.0f / x;}); }
    Tensor sqrt() const { return unary([](float x){return std::sqrt(x);}, [](float x, float){return 0.5f / std::sqrt(x);}); }
    Tensor abs() const  { return unary([](float x){return std::fabs(x);}, [](float x, float){return x < 0 ? -1.0f : 1.0f;}); }
    Tensor neg() const  { return unary([](float x){return -x;},           [](float, float){return -1.0f;}); }
    Tensor sigmoid() const {
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++) {
            float s = 1.0f / (1.0f + std::exp(-impl_->data[i]));
            p->data[i] = s;
        }
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            p->backward_fn = [src](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                for (size_t i = 0; i < self.data.size(); i++) {
                    float s = self.data[i];
                    src->grad->data[i] += self.grad->data[i] * s * (1.0f - s);
                }
            };
        }
        return Tensor(p);
    }

    Tensor pow(float e) const {
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++) p->data[i] = std::pow(impl_->data[i], e);
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            p->backward_fn = [src, e](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                for (size_t i = 0; i < self.data.size(); i++)
                    src->grad->data[i] += self.grad->data[i] * e * std::pow(src->data[i], e - 1.0f);
            };
        }
        return Tensor(p);
    }

    Tensor clamp(float lo, float hi) const {
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++)
            p->data[i] = std::max(lo, std::min(hi, impl_->data[i]));
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            p->backward_fn = [src, lo, hi](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                for (size_t i = 0; i < self.data.size(); i++) {
                    float v = src->data[i];
                    if (v > lo && v < hi) src->grad->data[i] += self.grad->data[i];
                }
            };
        }
        return Tensor(p);
    }
    Tensor clamp_min(float lo) const { return clamp(lo, std::numeric_limits<float>::max()); }
    Tensor clamp_max(float hi) const { return clamp(std::numeric_limits<float>::lowest(), hi); }

    // ---- Reductions ----
    Tensor sum() const {
        float s = 0.0f;
        for (auto v : impl_->data) s += v;
        auto p = std::make_shared<TensorImpl>(std::vector<float>{s}, Shape{});
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            p->backward_fn = [src](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                float g = self.grad->data[0];
                for (auto& v : src->grad->data) v += g;
            };
        }
        return Tensor(p);
    }

    Tensor sum(int64_t dim) const {
        if (dim < 0) dim += impl_->shape.size();
        Shape out_shape;
        for (int i = 0; i < (int)impl_->shape.size(); i++)
            if (i != dim) out_shape.push_back(impl_->shape[i]);
        if (out_shape.empty()) out_shape.push_back(1);
        auto p = std::make_shared<TensorImpl>(out_shape);
        Shape strides(impl_->shape.size(), 1);
        for (int i = (int)impl_->shape.size() - 2; i >= 0; i--)
            strides[i] = strides[i + 1] * impl_->shape[i + 1];
        int64_t out_num = total(out_shape);
        std::fill(p->data.begin(), p->data.end(), 0.0f);
        for (int64_t i = 0; i < (int64_t)impl_->data.size(); i++) {
            // decode linear i -> coords, drop dim, re-encode to out linear
            int64_t rem = i;
            int64_t out_idx = 0;
            int64_t out_stride = 1;
            for (int k = (int)impl_->shape.size() - 1; k >= 0; k--) {
                int64_t coord = rem % impl_->shape[k];
                rem /= impl_->shape[k];
                if (k != dim) {
                    out_idx += coord * out_stride;
                    out_stride *= impl_->shape[k];
                }
            }
            p->data[out_idx] += impl_->data[i];
        }
        if (impl_->requires_grad && !is_no_grad()) {
            p->requires_grad = true;
            p->inputs.push_back(impl_);
            auto src = impl_;
            Shape src_shape = impl_->shape;
            p->backward_fn = [src, src_shape, dim, out_shape](TensorImpl& self) -> void {
                if (!src->requires_grad) return;
                src->ensure_grad();
                for (int64_t i = 0; i < (int64_t)src->data.size(); i++) {
                    int64_t rem = i;
                    int64_t out_idx = 0;
                    int64_t out_stride = 1;
                    for (int k = (int)src_shape.size() - 1; k >= 0; k--) {
                        int64_t coord = rem % src_shape[k];
                        rem /= src_shape[k];
                        if (k != dim) {
                            out_idx += coord * out_stride;
                            out_stride *= src_shape[k];
                        }
                    }
                    src->grad->data[i] += self.grad->data[out_idx];
                }
            };
        }
        return Tensor(p);
    }

    Tensor mean() const {
        Tensor s = sum();
        return s * (1.0f / static_cast<float>(numel()));
    }
    Tensor mean(int64_t dim) const {
        Tensor s = sum(dim);
        return s * (1.0f / static_cast<float>(impl_->shape[dim]));
    }

    // L2 norm over last dim (returns [N] from [N, D])
    Tensor linalg_vector_norm_lastdim() const {
        assert(impl_->shape.size() >= 1);
        int64_t last = impl_->shape.back();
        int64_t n = numel() / last;
        auto p = std::make_shared<TensorImpl>(Shape{n});
        for (int64_t i = 0; i < n; i++) {
            float s = 0.0f;
            for (int64_t j = 0; j < last; j++) {
                float v = impl_->data[i * last + j];
                s += v * v;
            }
            p->data[i] = std::sqrt(s);
        }
        // No autograd (used in densify under NoGrad)
        return Tensor(p);
    }

    // max along last dim -> returns [N] values
    Tensor max_lastdim() const {
        int64_t last = impl_->shape.back();
        int64_t n = numel() / last;
        auto p = std::make_shared<TensorImpl>(Shape{n});
        for (int64_t i = 0; i < n; i++) {
            float m = impl_->data[i * last];
            for (int64_t j = 1; j < last; j++)
                m = std::max(m, impl_->data[i * last + j]);
            p->data[i] = m;
        }
        return Tensor(p);
    }

    // Element-wise maximum with another tensor (same shape)
    Tensor maximum(const Tensor& o) const {
        assert(impl_->shape == o.impl_->shape);
        auto p = std::make_shared<TensorImpl>(impl_->shape);
        for (size_t i = 0; i < p->data.size(); i++)
            p->data[i] = std::max(impl_->data[i], o.impl_->data[i]);
        return Tensor(p); // no autograd (used in NoGrad)
    }

    // ---- 2D matmul (with autograd) ----
    // a: [m,k], b: [k,n] -> [m,n]
    static Tensor matmul(const Tensor& a, const Tensor& b) {
        assert(a.impl_->shape.size() == 2 && b.impl_->shape.size() == 2);
        int64_t m = a.impl_->shape[0], k = a.impl_->shape[1], n = b.impl_->shape[1];
        assert(b.impl_->shape[0] == k);
        auto p = std::make_shared<TensorImpl>(Shape{m, n});
        for (int64_t i = 0; i < m; i++)
            for (int64_t j = 0; j < n; j++) {
                float s = 0.0f;
                for (int64_t t = 0; t < k; t++)
                    s += a.impl_->data[i * k + t] * b.impl_->data[t * n + j];
                p->data[i * n + j] = s;
            }
        bool a_grad = a.impl_->requires_grad && !is_no_grad();
        bool b_grad = b.impl_->requires_grad && !is_no_grad();
        if (a_grad || b_grad) {
            p->requires_grad = true;
            if (a_grad) p->inputs.push_back(a.impl_);
            if (b_grad) p->inputs.push_back(b.impl_);
            p->backward_fn = [a_impl = a.impl_, b_impl = b.impl_, a_grad, b_grad, m, k, n](TensorImpl& self) -> void {
                const auto& g = self.grad->data;
                if (a_grad) {
                    a_impl->ensure_grad();
                    for (int64_t i = 0; i < m; i++)
                        for (int64_t t = 0; t < k; t++) {
                            float s = 0.0f;
                            for (int64_t j = 0; j < n; j++) s += g[i * n + j] * b_impl->data[t * n + j];
                            a_impl->grad->data[i * k + t] += s;
                        }
                }
                if (b_grad) {
                    b_impl->ensure_grad();
                    for (int64_t t = 0; t < k; t++)
                        for (int64_t j = 0; j < n; j++) {
                            float s = 0.0f;
                            for (int64_t i = 0; i < m; i++) s += g[i * n + j] * a_impl->data[i * k + t];
                            b_impl->grad->data[t * n + j] += s;
                        }
                }
            };
        }
        return Tensor(p);
    }

    // ---- Fill (in-place, no grad) ----
    void fill_(float v) { std::fill(impl_->data.begin(), impl_->data.end(), v); }

    // ---- In-place indexed copy (no grad; used for building tensors) ----
    // Copy row `src_row` of `src` into row `dst_row` of *this (2D).
    void copy_row_from(int64_t dst_row, const Tensor& src, int64_t src_row) {
        assert(impl_->shape.size() == 2 && src.impl_->shape.size() == 2);
        assert(impl_->shape[1] == src.impl_->shape[1]);
        std::memcpy(&impl_->data[dst_row * impl_->shape[1]],
                    &src.impl_->data[src_row * src.impl_->shape[1]],
                    sizeof(float) * impl_->shape[1]);
    }

    // Copy element block (general, no grad)
    void copy_block_from(int64_t dst_off, const Tensor& src, int64_t src_off, int64_t count) {
        std::memcpy(&impl_->data[dst_off], &src.impl_->data[src_off], sizeof(float) * count);
    }

    // ---- Comparison -> bool vector (first dim) ----
    std::vector<uint8_t> gt_mask(float thresh) const {
        std::vector<uint8_t> m(impl_->data.size());
        for (size_t i = 0; i < m.size(); i++) m[i] = impl_->data[i] > thresh ? 1 : 0;
        return m;
    }
    std::vector<uint8_t> lt_mask(float thresh) const {
        std::vector<uint8_t> m(impl_->data.size());
        for (size_t i = 0; i < m.size(); i++) m[i] = impl_->data[i] < thresh ? 1 : 0;
        return m;
    }

    // ---- Access impl (for ops that need raw data) ----
    std::shared_ptr<TensorImpl> impl() const { return impl_; }

private:
    std::shared_ptr<TensorImpl> impl_;
};

// ---- Free function convenience ----
inline Tensor operator*(float s, const Tensor& t) { return t * s; }
inline Tensor operator+(float s, const Tensor& t) { return t + s; }
inline Tensor operator-(float s, const Tensor& t) { return Tensor::full(t.sizes(), s) - t; }

// logit(x) = log(x / (1 - x)) — used for opacity init
inline Tensor logit(float x) {
    return Tensor::scalar(std::log(x / (1.0f - x)));
}

// where(cond, a, b) element-wise (no autograd; used in NoGrad contexts)
inline Tensor where(const std::vector<uint8_t>& cond, const Tensor& a, const Tensor& b) {
    assert(a.sizes() == b.sizes());
    auto p = std::make_shared<TensorImpl>(a.sizes());
    for (size_t i = 0; i < p->data.size(); i++)
        p->data[i] = cond[i] ? a.impl()->data[i] : b.impl()->data[i];
    return Tensor(p);
}

} // namespace mini

#endif // MINI_TENSOR_H
