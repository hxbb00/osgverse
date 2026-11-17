#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_set>
#include <osg/Math>
#include <osg/Notify>
#include "GaussianSimplifier.h"
#include "Math.h"
#include "3rdparty/Eigen/Dense"
#include "3rdparty/nanoflann.hpp"

using namespace osgVerse;

namespace
{
    const double TWO_PI_POW_1P5 = std::pow(2.0 * osg::PI, 1.5);
    const float LOG_2PI = (float)std::log(2.0 * osg::PI);
    const float SH_C0 = 0.28209479177387814f;
    const float MIN_MASS = 1e-12f;

    inline float clampValue(float v, float lo, float hi)
    { return v < lo ? lo : (v > hi ? hi : v); }

    inline float logAddExp(float a, float b)
    {
        float m = (a > b) ? a : b;
        return m + std::log(std::exp(a - m) + std::exp(b - m));
    }

    inline int restCoeffCount(int degree)
    { return (degree >= 3) ? 15 : ((degree >= 2) ? 8 : ((degree >= 1) ? 3 : 0)); }

    inline int degreeFromShDim(int shDim)
    {
        if (shDim >= 48) return 3; if (shDim >= 27) return 2; if (shDim >= 12) return 1;
        return 0;
    }

    // Position of higher-band coefficient 'c' inside the SH getter groups:
    // group 0 holds (dc, c0, c1, c2), group N>0 holds (c{4N-1} .. c{4N+2})
    inline int coeffGroup(int c) { return (c < 3) ? 0 : ((c + 1) / 4); }
    inline int coeffPosition(int c) { return (c < 3) ? (c + 1) : ((c + 1) % 4); }

    void quatToMatrix(const osg::Vec4& q, float* m)
    {
        float w = (float)q[0], x = (float)q[1], y = (float)q[2], z = (float)q[3];
        float ww = w * w, xx = x * x, yy = y * y, zz = z * z;
        float wx = w * x, wy = w * y, wz = w * z, xy = x * y, xz = x * z, yz = y * z;
        m[0] = 1.0f - 2.0f * (yy + zz); m[1] = 2.0f * (xy - wz); m[2] = 2.0f * (xz + wy);
        m[3] = 2.0f * (xy + wz); m[4] = 1.0f - 2.0f * (xx + zz); m[5] = 2.0f * (yz - wx);
        m[6] = 2.0f * (xz - wy); m[7] = 2.0f * (yz + wx); m[8] = 1.0f - 2.0f * (xx + yy);
    }

    osg::Vec4 matrixToQuat(const float* m)
    {
        float qw, qx, qy, qz; const float tr = m[0] + m[4] + m[8];
        if (tr > 0.0f)
        {
            float s = std::sqrt(tr + 1.0f) * 2.0f;
            qw = 0.25f * s; qx = (m[7] - m[5]) / s; qy = (m[2] - m[6]) / s; qz = (m[3] - m[1]) / s;
        }
        else if (m[0] > m[4] && m[0] > m[8])
        {
            float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
            qw = (m[7] - m[5]) / s; qx = 0.25f * s; qy = (m[1] + m[3]) / s; qz = (m[2] + m[6]) / s;
        }
        else if (m[4] > m[8])
        {
            float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
            qw = (m[2] - m[6]) / s; qx = (m[1] + m[3]) / s; qy = 0.25f * s; qz = (m[5] + m[7]) / s;
        }
        else
        {
            float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
            qw = (m[3] - m[1]) / s; qx = (m[2] + m[6]) / s; qy = (m[5] + m[7]) / s; qz = 0.25f * s;
        }

        float n = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        n = (n > 1e-12f) ? (1.0f / n) : 0.0f;
        return osg::Vec4(qw * n, qx * n, qy * n, qz * n);
    }

    // Sigma = R * diag(var) * R^T
    void matrixFromRotVar(const float* R, const osg::Vec3& var, float* s)
    {
        float vx = (float)var[0], vy = (float)var[1], vz = (float)var[2];
        float r00 = R[0], r01 = R[1], r02 = R[2];
        float r10 = R[3], r11 = R[4], r12 = R[5];
        float r20 = R[6], r21 = R[7], r22 = R[8];
        s[0] = r00 * r00 * vx + r01 * r01 * vy + r02 * r02 * vz;
        s[1] = r00 * r10 * vx + r01 * r11 * vy + r02 * r12 * vz;
        s[2] = r00 * r20 * vx + r01 * r21 * vy + r02 * r22 * vz;
        s[3] = s[1];
        s[4] = r10 * r10 * vx + r11 * r11 * vy + r12 * r12 * vz;
        s[5] = r10 * r20 * vx + r11 * r21 * vy + r12 * r22 * vz;
        s[6] = s[2]; s[7] = s[5];
        s[8] = r20 * r20 * vx + r21 * r21 * vy + r22 * r22 * vz;
    }

    inline double determinant3(const float* s)
    {
        return (double)s[0] * ((double)s[4] * s[8] - (double)s[5] * s[7]) -
               (double)s[1] * ((double)s[3] * s[8] - (double)s[5] * s[6]) +
               (double)s[2] * ((double)s[3] * s[7] - (double)s[4] * s[6]);
    }

    inline void sampleSplat(const osg::Vec3& mu, const float* R, const osg::Vec3& var,
                            const osg::Vec3& z, osg::Vec3& out)
    {
        float s0 = std::sqrt((float)var[0]) * (float)z[0];
        float s1 = std::sqrt((float)var[1]) * (float)z[1];
        float s2 = std::sqrt((float)var[2]) * (float)z[2];
        out[0] = mu[0] + R[0] * s0 + R[1] * s1 + R[2] * s2;
        out[1] = mu[1] + R[3] * s0 + R[4] * s1 + R[5] * s2;
        out[2] = mu[2] + R[6] * s0 + R[7] * s1 + R[8] * s2;
    }

    inline float gaussLogPdf(const osg::Vec3& x, const osg::Vec3& mu, const float* R,
                             const osg::Vec3& invVar, float logdet)
    {
        float dx = (float)(x[0] - mu[0]), dy = (float)(x[1] - mu[1]), dz = (float)(x[2] - mu[2]);
        float y0 = dx * R[0] + dy * R[3] + dz * R[6];
        float y1 = dx * R[1] + dy * R[4] + dz * R[7];
        float y2 = dx * R[2] + dy * R[5] + dz * R[8];
        float q = y0 * y0 * (float)invVar[0] + y1 * y1 * (float)invVar[1] +
                  y2 * y2 * (float)invVar[2];
        return -0.5f * (3.0f * LOG_2PI + logdet + q);
    }

    struct SimpleRandom
    {
        uint32_t state;
        explicit SimpleRandom(uint32_t seed)
        { state = seed ? seed : 0x9e3779b9u; }

        float next()
        {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            return (float)((state >> 8) * (1.0 / 16777216.0));
        }
    };

    void createGaussianSamples(int count, unsigned int seed, std::vector<osg::Vec3>& samples)
    {
        samples.resize(count); SimpleRandom rand(seed);
        for (int i = 0; i < count; ++i)
        {
            float u1 = std::max(rand.next(), 1e-12f), u2 = rand.next();
            float u3 = std::max(rand.next(), 1e-12f), u4 = rand.next();
            float r1 = std::sqrt(-2.0f * std::log(u1)), t1 = 2.0f * (float)osg::PI * u2;
            float r2 = std::sqrt(-2.0f * std::log(u3)), t2 = 2.0f * (float)osg::PI * u4;
            samples[i].set(r1 * std::cos(t1), r1 * std::sin(t1), r2 * std::cos(t2));
        }
    }

    struct SplatCache
    {
        std::vector<osg::Vec3> var, invVar;
        std::vector<float> logdet, mass;
        std::vector<float> rotMat;   // 9 floats per splat, row-major
        std::vector<float> sigma;    // 9 floats per splat, row-major
    };

    void buildCache(const GaussianSplatCloud& cloud, float eps, SplatCache& cache)
    {
        const size_t N = cloud.count;
        cache.var.resize(N); cache.invVar.resize(N);
        cache.logdet.resize(N); cache.mass.resize(N);
        cache.rotMat.resize(N * 9); cache.sigma.resize(N * 9);

#pragma omp parallel for
        for (int n = 0; n < (int)N; ++n)
        {
            const osg::Vec3& sc = cloud.scales[n];
            float vx = (float)sc[0] * (float)sc[0] + eps;
            float vy = (float)sc[1] * (float)sc[1] + eps;
            float vz = (float)sc[2] * (float)sc[2] + eps;
            cache.var[n].set(vx, vy, vz);
            cache.invVar[n].set(1.0f / vx, 1.0f / vy, 1.0f / vz);
            cache.logdet[n] = std::log(vx) + std::log(vy) + std::log(vz);
            cache.mass[n] = (float)(TWO_PI_POW_1P5 * cloud.opacities[n] *
                                    sc[0] * sc[1] * sc[2]) + MIN_MASS;

            float* R = &cache.rotMat[n * 9];
            quatToMatrix(cloud.rotations[n], R);
            matrixFromRotVar(R, cache.var[n], &cache.sigma[n * 9]);
        }
    }

    struct PointCloudAdaptor
    {
        const std::vector<osg::Vec3>* points;
        size_t kdtree_get_point_count() const { return points->size(); }
        float kdtree_get_pt(size_t idx, int dim) const { return (float)(*points)[idx][dim]; }
        template <class BBOX> bool kdtree_get_bbox(BBOX&) const { return false; }
    };
    typedef nanoflann::L2_Simple_Adaptor<float, PointCloudAdaptor> DistanceAdaptor;
    typedef nanoflann::KDTreeSingleIndexAdaptor<DistanceAdaptor, PointCloudAdaptor, 3> KdTree;

    void buildEdges(const GaussianSplatCloud& cloud, int k,
                    std::vector<std::pair<uint32_t, uint32_t>>& edges,
                    GaussianSimplifier::ProgressCallback callback)
    {
        const size_t N = cloud.count; edges.clear();
        if (N < 2) return;

        PointCloudAdaptor adaptor; adaptor.points = &cloud.positions;
        KdTree tree(3, adaptor, nanoflann::KDTreeSingleIndexAdaptorParams(10));
        tree.buildIndex();

        const size_t kk = std::min<size_t>((size_t)std::max(1, k) + 1, N);
        std::vector<uint32_t> indices(kk); std::vector<float> distances(kk);
        std::unordered_set<uint64_t> visited;
        visited.reserve(N * (size_t)std::max(1, k) / 2);

        for (size_t i = 0; i < N; ++i)
        {
            const osg::Vec3& p = cloud.positions[i];
            float query[3] = { (float)p[0], (float)p[1], (float)p[2] };
            size_t found = tree.knnSearch(query, kk, indices.data(), distances.data());
            for (size_t t = 0; t < found; ++t)
            {
                size_t j = indices[t]; if (j == i) continue;
                uint64_t u = (i < j) ? i : j, v = (i < j) ? j : i;
                uint64_t key = u * (uint64_t)N + v;
                if (!visited.insert(key).second) continue;
                edges.push_back(std::pair<uint32_t, uint32_t>((uint32_t)u, (uint32_t)v));
            }
            if (callback && (i & 0x3fff) == 0) callback("knn", i, N);
        }
    }

    float pairCost(size_t i, size_t j, const GaussianSplatCloud& cloud,
                   const SplatCache& cache, const std::vector<osg::Vec3>& samples,
                   const GaussianSimplifyOptions& opt)
    {
        const float wi = cache.mass[i], wj = cache.mass[j];
        const float W = (wi + wj > 0.0f) ? (wi + wj) : 1.0f;
        float pi = clampValue(wi / W, 1e-12f, 1.0f - 1e-12f), pj = 1.0f - pi;
        const float logPi = std::log(pi), logPj = std::log(pj);

        const osg::Vec3 &mui = cloud.positions[i], &muj = cloud.positions[j];
        float mm[3] = { (float)(pi * mui[0] + pj * muj[0]), (float)(pi * mui[1] + pj * muj[1]),
                        (float)(pi * mui[2] + pj * muj[2]) };
        float di[3] = { (float)mui[0] - mm[0], (float)mui[1] - mm[1], (float)mui[2] - mm[2] };
        float dj[3] = { (float)muj[0] - mm[0], (float)muj[1] - mm[1], (float)muj[2] - mm[2] };

        const float* si = &cache.sigma[i * 9]; const float* sj = &cache.sigma[j * 9];
        float Sigm[9];
        for (int a = 0; a < 9; ++a) Sigm[a] = pi * si[a] + pj * sj[a];
        Sigm[0] += pi * di[0] * di[0] + pj * dj[0] * dj[0];
        Sigm[1] += pi * di[0] * di[1] + pj * dj[0] * dj[1];
        Sigm[2] += pi * di[0] * di[2] + pj * dj[0] * dj[2];
        Sigm[3] += pi * di[1] * di[0] + pj * dj[1] * dj[0];
        Sigm[4] += pi * di[1] * di[1] + pj * dj[1] * dj[1];
        Sigm[5] += pi * di[1] * di[2] + pj * dj[1] * dj[2];
        Sigm[6] += pi * di[2] * di[0] + pj * dj[2] * dj[0];
        Sigm[7] += pi * di[2] * di[1] + pj * dj[2] * dj[1];
        Sigm[8] += pi * di[2] * di[2] + pj * dj[2] * dj[2];

        float s01 = 0.5f * (Sigm[1] + Sigm[3]), s02 = 0.5f * (Sigm[2] + Sigm[6]);
        float s12 = 0.5f * (Sigm[5] + Sigm[7]);
        Sigm[1] = Sigm[3] = s01; Sigm[2] = Sigm[6] = s02; Sigm[5] = Sigm[7] = s12;
        Sigm[0] += opt.epsCov; Sigm[4] += opt.epsCov; Sigm[8] += opt.epsCov;

        double det = determinant3(Sigm); if (det < 1e-30) det = 1e-30;
        float E_negLogQ = 0.5f * (3.0f * LOG_2PI + std::log((float)det) + 3.0f);

        const float* Ri = &cache.rotMat[i * 9]; const float* Rj = &cache.rotMat[j * 9];
        const osg::Vec3 &vi = cache.var[i], &vj = cache.var[j];
        double sumLogpOnI = 0.0, sumLogpOnJ = 0.0;
        for (size_t s = 0; s < samples.size(); ++s)
        {
            const osg::Vec3& z = samples[s];
            osg::Vec3 xi, xj;
            sampleSplat(mui, Ri, vi, z, xi); sampleSplat(muj, Rj, vj, z, xj);

            float logNiOnI = gaussLogPdf(xi, mui, Ri, cache.invVar[i], cache.logdet[i]);
            float logNjOnI = gaussLogPdf(xi, muj, Rj, cache.invVar[j], cache.logdet[j]);
            sumLogpOnI += logAddExp(logPi + logNiOnI, logPj + logNjOnI);

            float logNiOnJ = gaussLogPdf(xj, mui, Ri, cache.invVar[i], cache.logdet[i]);
            float logNjOnJ = gaussLogPdf(xj, muj, Rj, cache.invVar[j], cache.logdet[j]);
            sumLogpOnJ += logAddExp(logPi + logNiOnJ, logPj + logNjOnJ);
        }

        float Ei = (float)(sumLogpOnI / samples.size());
        float Ej = (float)(sumLogpOnJ / samples.size());
        double geo = pi * Ei + pj * Ej + E_negLogQ;

        double cSh = 0.0;
        if (opt.useSHCost && cloud.shDim > 0)
        {
            const float* shi = &cloud.sh[i * cloud.shDim];
            const float* shj = &cloud.sh[j * cloud.shDim];
            for (int c = 0; c < cloud.shDim; ++c)
            {
                double d = (double)shi[c] - shj[c]; cSh += d * d;
            }
        }
        return (float)(opt.lamGeo * geo + opt.lamSh * cSh);
    }

    bool mergeSplatPair(const GaussianSplatCloud& src, size_t i, size_t j,
                        const SplatCache& cache, GaussianSplatCloud& dst, size_t dstIndex,
                        const GaussianSimplifyOptions& opt)
    {
        const float wi = cache.mass[i], wj = cache.mass[j];
        const float W = (wi + wj > 0.0f) ? (wi + wj) : 1.0f;
        const osg::Vec3 &mui = src.positions[i], &muj = src.positions[j];

        osg::Vec3 mu((wi * mui[0] + wj * muj[0]) / W, (wi * mui[1] + wj * muj[1]) / W,
                     (wi * mui[2] + wj * muj[2]) / W);
        float di[3] = { (float)(mui[0] - mu[0]), (float)(mui[1] - mu[1]), (float)(mui[2] - mu[2]) };
        float dj[3] = { (float)(muj[0] - mu[0]), (float)(muj[1] - mu[1]), (float)(muj[2] - mu[2]) };

        const float* si = &cache.sigma[i * 9]; const float* sj = &cache.sigma[j * 9];
        float Sigm[9];
        for (int a = 0; a < 9; ++a) Sigm[a] = (wi * si[a] + wj * sj[a]) / W;
        Sigm[0] += (wi * di[0] * di[0] + wj * dj[0] * dj[0]) / W;
        Sigm[1] += (wi * di[0] * di[1] + wj * dj[0] * dj[1]) / W;
        Sigm[2] += (wi * di[0] * di[2] + wj * dj[0] * dj[2]) / W;
        Sigm[3] += (wi * di[1] * di[0] + wj * dj[1] * dj[0]) / W;
        Sigm[4] += (wi * di[1] * di[1] + wj * dj[1] * dj[1]) / W;
        Sigm[5] += (wi * di[1] * di[2] + wj * dj[1] * dj[2]) / W;
        Sigm[6] += (wi * di[2] * di[0] + wj * dj[2] * dj[0]) / W;
        Sigm[7] += (wi * di[2] * di[1] + wj * dj[2] * dj[1]) / W;
        Sigm[8] += (wi * di[2] * di[2] + wj * dj[2] * dj[2]) / W;

        float s01 = 0.5f * (Sigm[1] + Sigm[3]), s02 = 0.5f * (Sigm[2] + Sigm[6]);
        float s12 = 0.5f * (Sigm[5] + Sigm[7]);
        Sigm[1] = Sigm[3] = s01; Sigm[2] = Sigm[6] = s02; Sigm[5] = Sigm[7] = s12;
        Sigm[0] += 1e-8f; Sigm[4] += 1e-8f; Sigm[8] += 1e-8f;

        Eigen::Matrix3f A;
        A << Sigm[0], Sigm[1], Sigm[2], Sigm[3], Sigm[4], Sigm[5], Sigm[6], Sigm[7], Sigm[8];
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver(A);
        if (solver.info() != Eigen::Success) return false;

        const Eigen::Vector3f& values = solver.eigenvalues();      // ascending
        const Eigen::Matrix3f& vectors = solver.eigenvectors();    // columns
        float R[9] = { vectors(0, 2), vectors(0, 1), vectors(0, 0),
                       vectors(1, 2), vectors(1, 1), vectors(1, 0),
                       vectors(2, 2), vectors(2, 1), vectors(2, 0) };
        float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) +
                    R[2] * (R[3] * R[7] - R[4] * R[6]);
        if (det < 0.0f) { R[2] = -R[2]; R[5] = -R[5]; R[8] = -R[8]; }

        float l0 = std::max(values(2), 1e-18f), l1 = std::max(values(1), 1e-18f);
        float l2 = std::max(values(0), 1e-18f);
        dst.positions[dstIndex] = mu;
        dst.scales[dstIndex].set(std::sqrt(l0), std::sqrt(l1), std::sqrt(l2));
        dst.rotations[dstIndex] = matrixToQuat(R);

        // Union preserves the composited alpha of two coincident layers but makes the
        // merged splat more opaque, which enlarges its screen quad (k grows with alpha)
        // and reduces fragment discards. Average/max keep the per-splat cost flat.
        const float oi = src.opacities[i], oj = src.opacities[j];
        float opacity = oi + oj - oi * oj;
        if (opt.opacityMode == 1) opacity = 0.5f * (oi + oj);
        else if (opt.opacityMode == 2) opacity = std::max(oi, oj);
        dst.opacities[dstIndex] = clampValue(opacity, 0.0f, 1.0f);

        const float* shi = &src.sh[i * src.shDim];
        const float* shj = &src.sh[j * src.shDim];
        float* shd = &dst.sh[dstIndex * dst.shDim];
        for (int c = 0; c < src.shDim; ++c) shd[c] = (wi * shi[c] + wj * shj[c]) / W;
        return true;
    }

    void allocateCloud(GaussianSplatCloud& cloud, size_t count, int shDim)
    {
        cloud.count = count; cloud.shDim = shDim;
        cloud.positions.resize(count); cloud.scales.resize(count);
        cloud.rotations.resize(count); cloud.opacities.resize(count);
        cloud.sh.assign(count * (size_t)shDim, 0.0f);
    }

    void copySplat(const GaussianSplatCloud& src, size_t i, GaussianSplatCloud& dst, size_t j)
    {
        dst.positions[j] = src.positions[i]; dst.scales[j] = src.scales[i];
        dst.rotations[j] = src.rotations[i]; dst.opacities[j] = src.opacities[i];
        const float* s = &src.sh[i * src.shDim]; float* d = &dst.sh[j * dst.shDim];
        for (int c = 0; c < src.shDim; ++c) d[c] = s[c];
    }

    void subsetCloud(const GaussianSplatCloud& src, const std::vector<uint32_t>& keep,
                     GaussianSplatCloud& dst)
    {
        allocateCloud(dst, keep.size(), src.shDim);
#pragma omp parallel for
        for (int n = 0; n < (int)keep.size(); ++n) copySplat(src, keep[n], dst, n);
    }

    void pruneCloud(GaussianSplatCloud& cloud, float threshold)
    {
        const size_t N = cloud.count; if (N < 2) return;
        std::vector<float> ops(cloud.opacities);
        size_t mid = N / 2;
        std::nth_element(ops.begin(), ops.begin() + mid, ops.end());
        float thr = std::min(threshold, ops[mid]);
        std::vector<uint32_t> keep; keep.reserve(N);
        for (size_t i = 0; i < N; ++i)
            if (cloud.opacities[i] >= thr) keep.push_back((uint32_t)i);
        if (keep.size() == N) return;

        GaussianSplatCloud pruned; subsetCloud(cloud, keep, pruned);
        cloud = std::move(pruned);
    }

    void mergeCloud(const GaussianSplatCloud& src,
                    const std::vector<std::pair<uint32_t, uint32_t>>& pairs,
                    const SplatCache& cache, GaussianSplatCloud& dst,
                    const GaussianSimplifyOptions& opt)
    {
        const size_t N = src.count;
        std::vector<uint8_t> used(N, 0);
        for (size_t p = 0; p < pairs.size(); ++p)
            { used[pairs[p].first] = 1; used[pairs[p].second] = 1; }

        std::vector<uint32_t> keep; keep.reserve(N - pairs.size() * 2);
        for (size_t i = 0; i < N; ++i) if (!used[i]) keep.push_back((uint32_t)i);

        allocateCloud(dst, keep.size() + pairs.size(), src.shDim);
#pragma omp parallel for
        for (int n = 0; n < (int)keep.size(); ++n) copySplat(src, keep[n], dst, n);
#pragma omp parallel for
        for (int p = 0; p < (int)pairs.size(); ++p)
        {
            if (!mergeSplatPair(src, pairs[p].first, pairs[p].second, cache, dst,
                                keep.size() + (size_t)p, opt))
                copySplat(src, pairs[p].first, dst, keep.size() + (size_t)p);
        }
    }
}

bool GaussianSplatCloud::valid() const
{
    if (count == 0) return false;
    if (shDim != 3 && shDim != 12 && shDim != 27 && shDim != 48) return false;
    if (positions.size() != count || scales.size() != count) return false;
    if (rotations.size() != count || opacities.size() != count) return false;
    return sh.size() == count * (size_t)shDim;
}

bool GaussianSimplifier::simplify(GaussianSplatCloud& cloud, const GaussianSimplifyOptions& optIn,
                                  ProgressCallback callback)
{
    if (!cloud.valid()) return false;
    GaussianSimplifyOptions opt = optIn;
    opt.ratio = clampValue(opt.ratio, 1e-6f, 1.0f - 1e-6f);
    opt.mergeCapRatio = clampValue(opt.mergeCapRatio, 0.01f, 0.5f);
    opt.kNeighbors = std::max(1, opt.kNeighbors);
    opt.sampleCount = std::max(1, opt.sampleCount);
    opt.maxPasses = std::max(1, opt.maxPasses);

#ifdef _OPENMP
    if (opt.numThreads > 0) omp_set_num_threads(opt.numThreads);
#endif

    const size_t N0 = cloud.count;
    const size_t target = (size_t)std::max(1.0, std::ceil((double)N0 * opt.ratio));
    if (target >= N0) { if (callback) callback("done", cloud.count, target); return true; }

    pruneCloud(cloud, opt.opacityThreshold);
    if (cloud.count <= target) { if (callback) callback("done", cloud.count, target); return true; }

    std::vector<osg::Vec3> samples;
    createGaussianSamples(opt.sampleCount, opt.seed, samples);

    SplatCache cache;
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    for (int iteration = 0; cloud.count > target && iteration < opt.maxPasses; ++iteration)
    {
        buildCache(cloud, opt.epsCov, cache);
        buildEdges(cloud, opt.kNeighbors, edges, callback);
        if (edges.empty()) break;

        std::vector<float> cost(edges.size());
        const int numEdges = (int)edges.size();
#pragma omp parallel for
        for (int e = 0; e < numEdges; ++e)
            cost[e] = pairCost(edges[e].first, edges[e].second, cloud, cache, samples, opt);

        const size_t mergesNeeded = cloud.count - target;
        const size_t mergeCap = std::max<size_t>(1, (size_t)std::floor(opt.mergeCapRatio * N0));
        const size_t limit = std::min(mergesNeeded, mergeCap);

        std::vector<uint32_t> order; order.reserve(edges.size());
        for (uint32_t e = 0; e < (uint32_t)edges.size(); ++e)
            if (std::isfinite(cost[e])) order.push_back(e);
        if (order.empty()) break;

        std::sort(order.begin(), order.end(), [&cost, &edges](uint32_t a, uint32_t b) {
            if (cost[a] != cost[b]) return cost[a] < cost[b];
            if (edges[a].first != edges[b].first) return edges[a].first < edges[b].first;
            return edges[a].second < edges[b].second;
        });

        std::vector<uint8_t> used(cloud.count, 0);
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        pairs.reserve(std::min(limit, order.size()));
        for (size_t t = 0; t < order.size() && pairs.size() < limit; ++t)
        {
            const std::pair<uint32_t, uint32_t>& e = edges[order[t]];
            if (used[e.first] || used[e.second]) continue;
            used[e.first] = used[e.second] = 1; pairs.push_back(e);
        }
        if (pairs.empty()) break;

        GaussianSplatCloud next;
        mergeCloud(cloud, pairs, cache, next, opt);
        cloud = std::move(next);

        if (opt.verbose)
        {
            OSG_NOTICE << "[GaussianSimplifier] Pass " << iteration + 1 << ": "
                       << edges.size() << " edges, " << pairs.size() << " merges, "
                       << cloud.count << " / " << target << " splats" << std::endl;
        }
        if (callback) callback("pass", cloud.count, target);
    }

    if (callback) callback("done", cloud.count, target);
    return true;
}

bool GaussianSimplifier::fromGeometry(GaussianGeometry& geom, GaussianSplatCloud& cloud)
{
    const int N = geom.getNumSplats(); if (N <= 0) return false;
    allocateCloud(cloud, (size_t)N, 3);

    osg::Vec4* pos4 = geom.getPosition4();
    osg::Vec3* pos3 = pos4 ? NULL : geom.getPosition3();
    if (!pos4 && !pos3) return false;

    for (int i = 0; i < N; ++i)
    {
        if (pos4)
        {
            cloud.positions[i].set(pos4[i][0], pos4[i][1], pos4[i][2]);
            cloud.opacities[i] = clampValue((float)pos4[i][3], 0.0f, 1.0f);
        }
        else
            cloud.positions[i] = pos3[i];
    }

    if (!pos4)
    {
        osg::ref_ptr<osg::FloatArray> alpha = geom.getAlpha();
        if (alpha.valid() && alpha->size() >= (size_t)N)
        {
            for (int i = 0; i < N; ++i)
                cloud.opacities[i] = clampValue((*alpha)[i], 0.0f, 1.0f);
        }
    }

    osg::ref_ptr<osg::Vec3Array> scale = geom.getScale();
    osg::ref_ptr<osg::Vec4Array> rotation = geom.getRotation();
    if (!scale.valid() || scale->size() < (size_t)N) return false;
    if (!rotation.valid() || rotation->size() < (size_t)N) return false;

    for (int i = 0; i < N; ++i)
    {
        const osg::Vec3& s = (*scale)[i];
        cloud.scales[i].set(std::max((float)s[0], 1e-12f), std::max((float)s[1], 1e-12f),
                            std::max((float)s[2], 1e-12f));
        osg::Vec4 q = (*rotation)[i];  // (w, x, y, z) for instancing methods
        if (geom.getRenderMethod() == GaussianGeometry::GEOMETRY_SHADER)
            q.set(q[3], q[0], q[1], q[2]);  // that method stores (x, y, z, w)
        float n = std::sqrt((float)(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]));
        if (n > 1e-12f) { q /= n; } else { q.set(1.0f, 0.0f, 0.0f, 0.0f); }
        cloud.rotations[i] = q;
    }

    const int degree = std::min(std::max(geom.getShDegrees(), 0), 3);
    const int rest = restCoeffCount(degree);
    cloud.shDim = 3 + rest * 3;
    cloud.sh.assign(cloud.count * (size_t)cloud.shDim, 0.0f);

    osg::ref_ptr<osg::Vec4Array> groups[4][3];  // [group][0: red, 1: green, 2: blue]
    for (int g = 0; g < 4; ++g) for (int c = 0; c < 3; ++c) groups[g][c] = NULL;
    groups[0][0] = geom.getShRed(0); groups[0][1] = geom.getShGreen(0);
    groups[0][2] = geom.getShBlue(0);
    if (!groups[0][0].valid() || !groups[0][1].valid() || !groups[0][2].valid())
        return false;

    for (int i = 0; i < N; ++i)
    {   // DC is linear RGB, matching the color space used by the merge cost
        cloud.sh[i * cloud.shDim + 0] = (float)(0.5 + SH_C0 * (*groups[0][0])[i][0]);
        cloud.sh[i * cloud.shDim + 1] = (float)(0.5 + SH_C0 * (*groups[0][1])[i][0]);
        cloud.sh[i * cloud.shDim + 2] = (float)(0.5 + SH_C0 * (*groups[0][2])[i][0]);
    }

    for (int c = 0; c < rest; ++c)
    {
        const int g = coeffGroup(c), pos = coeffPosition(c);
        if (!groups[g][0].valid()) groups[g][0] = geom.getShRed(g);
        if (!groups[g][1].valid()) groups[g][1] = geom.getShGreen(g);
        if (!groups[g][2].valid()) groups[g][2] = geom.getShBlue(g);
        if (!groups[g][0].valid() || !groups[g][1].valid() || !groups[g][2].valid()) continue;
        if (groups[g][0]->size() < (size_t)N || groups[g][1]->size() < (size_t)N ||
            groups[g][2]->size() < (size_t)N) continue;

        for (int i = 0; i < N; ++i)
        {
            size_t base = i * cloud.shDim + 3 + c * 3;
            cloud.sh[base + 0] = (float)(*groups[g][0])[i][pos];
            cloud.sh[base + 1] = (float)(*groups[g][1])[i][pos];
            cloud.sh[base + 2] = (float)(*groups[g][2])[i][pos];
        }
    }
    return true;
}

osg::ref_ptr<GaussianGeometry> GaussianSimplifier::toGeometry(const GaussianSplatCloud& cloud,
                                                              GaussianGeometry::RenderMethod method)
{
    if (!cloud.valid()) return osg::ref_ptr<GaussianGeometry>();

    const int N = (int)cloud.count;
    const int degree = degreeFromShDim(cloud.shDim);
    const int rest = restCoeffCount(degree);

    osg::ref_ptr<osg::Vec3Array> pos = new osg::Vec3Array(N);
    osg::ref_ptr<osg::Vec3Array> scale = new osg::Vec3Array(N);
    osg::ref_ptr<osg::Vec4Array> rotation = new osg::Vec4Array(N);
    osg::ref_ptr<osg::FloatArray> alpha = new osg::FloatArray(N);
    for (int i = 0; i < N; ++i)
    {
        (*pos)[i] = cloud.positions[i]; (*scale)[i] = cloud.scales[i];
        const osg::Vec4& q = cloud.rotations[i];  // (w, x, y, z) -> setter wants (x, y, z, w)
        (*rotation)[i].set(q[1], q[2], q[3], q[0]);
        (*alpha)[i] = cloud.opacities[i];
    }

    osg::ref_ptr<osg::Vec4Array> groups[4][3];
    for (int g = 0; g < 4; ++g)
        for (int c = 0; c < 3; ++c)
            { groups[g][c] = new osg::Vec4Array; groups[g][c]->assign(N, osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); }

    for (int i = 0; i < N; ++i)
    {   // DC back to raw SH coefficients for the renderer
        (*groups[0][0])[i][0] = (cloud.sh[i * cloud.shDim + 0] - 0.5f) / SH_C0;
        (*groups[0][1])[i][0] = (cloud.sh[i * cloud.shDim + 1] - 0.5f) / SH_C0;
        (*groups[0][2])[i][0] = (cloud.sh[i * cloud.shDim + 2] - 0.5f) / SH_C0;
    }
    for (int c = 0; c < rest; ++c)
    {
        const int g = coeffGroup(c), p = coeffPosition(c);
        for (int i = 0; i < N; ++i)
        {
            size_t base = i * cloud.shDim + 3 + c * 3;
            (*groups[g][0])[i][p] = cloud.sh[base + 0];
            (*groups[g][1])[i][p] = cloud.sh[base + 1];
            (*groups[g][2])[i][p] = cloud.sh[base + 2];
        }
    }

    osg::ref_ptr<GaussianGeometry> result = new GaussianGeometry(method);
    result->setShDegrees(degree);
    result->setPosition(pos.get());
    result->setScaleAndRotation(scale.get(), rotation.get(), alpha.get());
    const int maxGroup = (rest > 0) ? coeffGroup(rest - 1) : 0;
    for (int g = 0; g <= maxGroup; ++g)
    {
        result->setShRed(g, groups[g][0].get());
        result->setShGreen(g, groups[g][1].get());
        result->setShBlue(g, groups[g][2].get());
    }
    result->finalize();
    return result;
}

osg::ref_ptr<GaussianGeometry> GaussianSimplifier::simplifyGeometry(
    GaussianGeometry& geom, const GaussianSimplifyOptions& opt, ProgressCallback callback)
{
    GaussianSplatCloud cloud;
    if (!fromGeometry(geom, cloud)) return osg::ref_ptr<GaussianGeometry>();
    if (!simplify(cloud, opt, callback)) return osg::ref_ptr<GaussianGeometry>();

    osg::ref_ptr<GaussianGeometry> result = toGeometry(cloud, geom.getRenderMethod());
    if (result.valid()) result->setName(geom.getName());
    return result;
}
