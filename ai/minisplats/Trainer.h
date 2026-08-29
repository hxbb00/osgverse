// mini/Trainer.h — High-level training pipeline.
//
// Takes an io::Dataset (Eigen intrinsics/extrinsics + osg::Image images + sparse
// points) and returns io::GaussianOutput (std::vector<Eigen::Vector3f> attribute
// arrays).  This is the "input → train → output" wrapper around the proven
// mini Tensor + autograd + ops framework.
//
// Convention: viewmat is world-to-camera, OpenCV style (+Z forward, Y down).
// If your data is camera-to-world, invert it before filling Camera::viewmat.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_TRAINER_H
#define MINI_TRAINER_H

#include "Tensor.h"
#include "Optimizer.h"
#include "Geometry.h"
#include "ops/SphericalHarmonics.h"
#include "ops/ProjectGaussians.h"
#include "ops/RasterizeGaussians.h"
#include "ops/Ssim.h"
#include "io/Types.h"
#include "io/Convert.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <random>

namespace mini {

// ---- Configuration ----
struct TrainConfig {
    int   numIters   = 30000;
    int   shDegree   = 3;
    float lrMeans    = 1.6e-4f;
    float lrMeansFinal = 1.6e-6f;
    float lrScales   = 5e-3f;
    float lrQuats    = 1e-3f;
    float lrFeatures = 2.5e-3f;
    float lrOpacities = 5e-2f;
    float ssimWeight = 0.2f;
    int   verboseEvery = 0;   // 0 = silent, else print every N steps
};

// ---- Internal helpers ----
namespace detail {

// Number of SH bases for a given degree: (degree+1)^2
inline int numShBases(int degree) { return (degree + 1) * (degree + 1); }

// Brute-force KNN scale initialization.
// For each point, find the 3 nearest neighbors, compute average distance,
// and use log(avg_dist) as the initial log-scale (all 3 axes).
// Matches OpenSplat's kdtree_tensor.cpp logic.
inline Tensor initScalesKNN(const Tensor& means, int k = 3) {
    int N = (int)means.size(0);
    const float* mp = means.data_ptr();
    Tensor scales = Tensor::empty({N, 3});
    float* sp = scales.data_ptr();

    for (int i = 0; i < N; i++) {
        std::vector<float> dists;
        dists.reserve(N - 1);
        for (int j = 0; j < N; j++) {
            if (j == i) continue;
            float dx = mp[i*3]   - mp[j*3];
            float dy = mp[i*3+1] - mp[j*3+1];
            float dz = mp[i*3+2] - mp[j*3+2];
            dists.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
        }
        std::sort(dists.begin(), dists.end());
        float sum = 0.0f;
        int cnt = std::min(k, (int)dists.size());
        for (int j = 0; j < cnt; j++) sum += dists[j];
        float avgDist = cnt > 0 ? sum / cnt : 1e-7f;
        avgDist = std::max(avgDist, 1e-7f);
        float logScale = std::log(avgDist);
        sp[i*3]   = logScale;
        sp[i*3+1] = logScale;
        sp[i*3+2] = logScale;
    }
    return scales;
}

// Pre-processed camera data (tensors, computed once at start).
struct CameraTensors {
    Tensor viewmat;    // {4,4}
    Tensor fullProj;   // {4,4}
    Tensor image;      // {H,W,3} RGB float
    Tensor viewDirs;   // {N,3} (filled per-step since N may change)
    float fx, fy, cx, cy;
    int W, H;
    Eigen::Vector3f camPos; // world-space camera position
};

} // namespace detail

// ---- Main training function ----
inline io::GaussianOutput train(const io::Dataset& dataset, const TrainConfig& cfg = {}) {
    // ---- 1. Validate input ----
    const int numCameras = (int)dataset.cameras.size();
    const int numPoints  = (int)dataset.points.size();
    if (numCameras == 0 || numPoints == 0)
        return {};

    const int K = detail::numShBases(cfg.shDegree);
    const float SH_C0 = 0.28209479177387814f;

    // ---- 2. Initialize learnable parameters ----
    // Means: from input points
    Tensor means = Tensor::empty({numPoints, 3});
    for (int i = 0; i < numPoints; i++) {
        means.data_ptr()[i*3]   = dataset.points[i].position.x();
        means.data_ptr()[i*3+1] = dataset.points[i].position.y();
        means.data_ptr()[i*3+2] = dataset.points[i].position.z();
    }
    means.requires_grad_(true);

    // Scales: log(avg KNN distance)
    Tensor scales = detail::initScalesKNN(means, 3);
    scales.requires_grad_(true);

    // Quaternions: random unit quaternions
    Tensor quats = randomQuatTensor(numPoints);
    quats.requires_grad_(true);

    // SH features: DC from point colors, rest = 0
    Tensor features = Tensor::zeros({numPoints, K, 3});
    for (int i = 0; i < numPoints; i++) {
        // rgb2sh: (color - 0.5) / SH_C0
        features.data_ptr()[i*K*3]   = (dataset.points[i].color.x() - 0.5f) / SH_C0;
        features.data_ptr()[i*K*3+1] = (dataset.points[i].color.y() - 0.5f) / SH_C0;
        features.data_ptr()[i*K*3+2] = (dataset.points[i].color.z() - 0.5f) / SH_C0;
    }
    features.requires_grad_(true);

    // Opacities: logit(0.1)
    Tensor opacities = Tensor::full({numPoints, 1}, std::log(0.1f / 0.9f));
    opacities.requires_grad_(true);

    // ---- 3. Setup optimizers ----
    Adam opt_m(&means,    {cfg.lrMeans});
    Adam opt_s(&scales,   {cfg.lrScales});
    Adam opt_q(&quats,    {cfg.lrQuats});
    Adam opt_f(&features, {cfg.lrFeatures});
    Adam opt_o(&opacities,{cfg.lrOpacities});
    ExponentialScheduler sched_m(&opt_m, cfg.lrMeans, cfg.lrMeansFinal, cfg.numIters);

    // ---- 4. Pre-process cameras ----
    std::vector<detail::CameraTensors> cams(numCameras);
    Tensor bgColor = Tensor::from_blob(std::vector<float>{0.0f, 0.0f, 0.0f}.data(), {3});
    Tensor ssimWindow = ops::createGaussianWindow(11, 3, 1.5f);

    for (int c = 0; c < numCameras; c++) {
        const auto& cam = dataset.cameras[c];
        cams[c].viewmat = io::toTensor(cam.viewmat);

        float fx = cam.K(0, 0), fy = cam.K(1, 1);
        float cx = cam.K(0, 2), cy = cam.K(1, 2);
        int W = cam.image->s(), H = cam.image->t();
        float fovX = 2.0f * std::atan(static_cast<float>(W) / (2.0f * fx));
        float fovY = 2.0f * std::atan(static_cast<float>(H) / (2.0f * fy));
        Tensor projMat = projectionMatrix(0.001f, 1000.0f, fovX, fovY);
        cams[c].fullProj = Tensor::matmul(projMat, cams[c].viewmat);
        cams[c].image = io::imageToTensor(*cam.image);
        cams[c].fx = fx; cams[c].fy = fy;
        cams[c].cx = cx; cams[c].cy = cy;
        cams[c].W = W;   cams[c].H = H;
        cams[c].camPos = io::cameraPosition(cam.viewmat);
    }

    // ---- 5. Training loop ----
    std::mt19937 rng(42);

    for (int step = 0; step < cfg.numIters; step++) {
        // Update means LR
        sched_m.step(step);

        // Pick random camera
        int camIdx = (int)(rng() % numCameras);
        auto& cam = cams[camIdx];
        int N = (int)means.size(0);
        int W = cam.W, H = cam.H;

        // Progressive SH degree
        int shInterval = std::max(1, cfg.numIters / (cfg.shDegree + 1));
        int degToUse = std::min(step / shInterval, cfg.shDegree);

        // --- Forward ---
        Tensor expScales, sigOpacities, normQuats;
        {
            NoGradGuard ng;
            expScales = scales.exp();
            sigOpacities = opacities.sigmoid();
            normQuats = normalizeQuats(quats);
        }

        // Compute view directions: (means - camPos) normalized
        Tensor viewDirs = Tensor::empty({N, 3});
        {
            NoGradGuard ng;
            for (int i = 0; i < N; i++) {
                float dx = means.data_ptr()[i*3]   - cam.camPos.x();
                float dy = means.data_ptr()[i*3+1] - cam.camPos.y();
                float dz = means.data_ptr()[i*3+2] - cam.camPos.z();
                float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (len < 1e-7f) len = 1e-7f;
                viewDirs.data_ptr()[i*3]   = dx / len;
                viewDirs.data_ptr()[i*3+1] = dy / len;
                viewDirs.data_ptr()[i*3+2] = dz / len;
            }
        }

        // Projection (manual forward)
        auto proj = ops::projectGaussiansForward(
            means, expScales, 1.0f, normQuats, cam.viewmat, cam.fullProj,
            cam.fx, cam.fy, cam.cx, cam.cy, H, W);

        // Skip if nothing is visible
        bool anyVisible = false;
        for (int i = 0; i < N; i++) {
            if (proj.radii.data_ptr()[i] > 0) { anyVisible = true; break; }
        }
        if (!anyVisible) continue;

        proj.xys.requires_grad_(true);
        proj.conics.requires_grad_(true);
        proj.camDepths.requires_grad_(true);

        // SH (autograd through features)
        Tensor colors = ops::sphericalHarmonics(cfg.shDegree, degToUse, viewDirs, features);
        colors = (colors + 0.5f).clamp_min(0.0f);

        sigOpacities.requires_grad_(true);

        // Rasterize (autograd)
        Tensor rgb = ops::rasterizeGaussians(
            W, H, proj.xys, proj.conics, colors, sigOpacities,
            bgColor, proj.cov2d, proj.camDepths);
        rgb = rgb.clamp_max(1.0f);

        // Loss: L1 + SSIM
        Tensor diff = rgb - cam.image;
        Tensor l1Loss = diff.abs().mean();
        Tensor loss = l1Loss;
        if (cfg.ssimWeight > 0) {
            Tensor sLoss = ops::ssimLoss(rgb, cam.image, ssimWindow);
            loss = l1Loss + cfg.ssimWeight * sLoss;
        }

        // --- Backward ---
        means.zero_grad(); scales.zero_grad(); quats.zero_grad();
        features.zero_grad(); opacities.zero_grad();
        proj.xys.zero_grad(); proj.conics.zero_grad(); proj.camDepths.zero_grad();

        loss.backward();

        // Manual projection backward
        Tensor v_depths = Tensor::zeros({N});
        auto v = ops::projectGaussiansBackward(
            proj.ctx, proj.xys.grad(), proj.conics.grad(), v_depths);

        // Chain rule for exp(scales) and sigmoid(opacities)
        Tensor v_scales_raw = v.v_scales * expScales;
        Tensor v_opac_raw = sigOpacities.grad() * sigOpacities * (1.0f - sigOpacities);

        // Copy gradients into params (projection is manual, not in autograd graph)
        means.impl()->ensure_grad();
        scales.impl()->ensure_grad();
        quats.impl()->ensure_grad();
        opacities.impl()->ensure_grad();
        std::memcpy(means.grad().data_ptr(),     v.v_means.data_ptr(),   sizeof(float) * means.numel());
        std::memcpy(scales.grad().data_ptr(),    v_scales_raw.data_ptr(),sizeof(float) * scales.numel());
        std::memcpy(quats.grad().data_ptr(),     v.v_quats.data_ptr(),   sizeof(float) * quats.numel());
        std::memcpy(opacities.grad().data_ptr(), v_opac_raw.data_ptr(),  sizeof(float) * opacities.numel());

        // Adam step
        opt_m.step(); opt_s.step(); opt_q.step(); opt_f.step(); opt_o.step();

        // Verbose
        if (cfg.verboseEvery > 0 && (step % cfg.verboseEvery == 0 || step == cfg.numIters - 1)) {
            printf("step %5d  loss = %.6f  l1 = %.6f  gaussians = %d\n",
                   step, loss.item(), l1Loss.item(), N);
            fflush(stdout);
        }
    }

    // ---- 6. Extract results ----
    io::GaussianOutput out;
    int N = (int)means.size(0);
    out.means.resize(N);
    out.scales.resize(N);
    out.rotations.resize(N);
    out.colors.resize(N);
    out.opacities.resize(N);
    out.shDegree = cfg.shDegree;
    out.shCoeffs.resize(N * K * 3);

    Tensor expScales, sigOpacities, normQuats;
    {
        NoGradGuard ng;
        expScales = scales.exp();
        sigOpacities = opacities.sigmoid();
        normQuats = normalizeQuats(quats);
    }

    const float* mp = means.data_ptr();
    const float* sp = expScales.data_ptr();
    const float* qp = normQuats.data_ptr();
    const float* fp = features.data_ptr();
    const float* op = sigOpacities.data_ptr();

    for (int i = 0; i < N; i++) {
        out.means[i]     = Eigen::Vector3f(mp[i*3], mp[i*3+1], mp[i*3+2]);
        out.scales[i]    = Eigen::Vector3f(sp[i*3], sp[i*3+1], sp[i*3+2]);
        out.rotations[i] = Eigen::Vector4f(qp[i*4], qp[i*4+1], qp[i*4+2], qp[i*4+3]);
        // SH DC term → RGB
        float r = std::clamp(fp[i*K*3]   * SH_C0 + 0.5f, 0.0f, 1.0f);
        float g = std::clamp(fp[i*K*3+1] * SH_C0 + 0.5f, 0.0f, 1.0f);
        float b = std::clamp(fp[i*K*3+2] * SH_C0 + 0.5f, 0.0f, 1.0f);
        out.colors[i]    = Eigen::Vector3f(r, g, b);
        out.opacities[i] = op[i];
        // Full SH coefficients
        std::memcpy(&out.shCoeffs[i * K * 3], &fp[i * K * 3], sizeof(float) * K * 3);
    }

    return out;
}

} // namespace mini

#endif // MINI_TRAINER_H
