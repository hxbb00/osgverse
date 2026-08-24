#include "3rdparty/freesplatter/free_splatter.h"
#include "3rdparty/freesplatter/splat.h"
#include "FreeSplatterEngine.h"
#include <osg/Notify>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>

#define FREESP_LOG(m) { OSG_WARN << "[FreeSplatterEngine] " << m << std::endl; return; }
#define FREESP_LOG_R(m, r) { OSG_WARN << "[FreeSplatterEngine] " << m << std::endl; return r; }
using namespace osgVerse;

namespace
{

    // ---------------------------------------------------------------------------
    // FreeSplatter::Impl
    // ---------------------------------------------------------------------------
    class FreeSplatterImpl : public osg::Referenced
    {
    public:
        free_splatter_ctx* ctx = nullptr;
        FreeSplatter::ModelInformation info;

        FreeSplatterImpl(const std::string& model, const std::string& device, int numThreads)
        {
            free_splatter_options* opts = free_splatter_options_new();
            if (!opts) FREESP_LOG("Failed to create free_splatter_options");

            if (!device.empty())
                free_splatter_options_set_device(opts, device.c_str());
            if (numThreads > 0)
                free_splatter_options_set_threads(opts, numThreads);

            ctx = free_splatter_load(model.c_str(), opts);
            free_splatter_options_free(opts);
            if (!ctx) FREESP_LOG("Out of memory loading model");

            if (const char* err = free_splatter_last_error(ctx))
            {
                std::string msg = err; free_splatter_free(ctx);
                ctx = nullptr; FREESP_LOG(msg);
            }

            free_splatter_geometry geo;
            free_splatter_geometry_of(ctx, &geo);
            info.image_width  = geo.image_width;
            info.image_height = geo.image_height;
            info.in_channels  = geo.in_channels;
            info.gaussian_channels = geo.gaussian_channels;
        }

        ~FreeSplatterImpl()
        { if (ctx) free_splatter_free(ctx); }
    };

}

// ---------------------------------------------------------------------------
// FreeSplatter
// ---------------------------------------------------------------------------
FreeSplatter::FreeSplatter(const std::string& model, int numThreads)
{ _internal = new FreeSplatterImpl(model, "", numThreads); }

FreeSplatter::FreeSplatter(const std::string& model, const std::string& device, int numThreads)
{ _internal = new FreeSplatterImpl(model, device, numThreads); }

FreeSplatter::~FreeSplatter()
{}

bool FreeSplatter::checkModelInformation(ModelInformation& info)
{
    FreeSplatterImpl* p = static_cast<FreeSplatterImpl*>(_internal.get());
    info = p->info; return true;
}

// ---------------------------------------------------------------------------
// Image preprocessing
// ---------------------------------------------------------------------------
std::vector<float> FreeSplatter::preprocessImage(const osg::Image& img, int targetSize)
{
    int w = img.s(), h = img.t(), n = img.getPixelSizeInBits() / 8;
    if (n < 3) FREESP_LOG_R("Image must have at least 3 channels", {});

    // Center-crop to square
    int s = std::min(w, h);
    int left = (w - s) / 2;
    int top  = (h - s) / 2;

    // Create image and resize to targetSize x targetSize
    osg::ref_ptr<osg::Image> crop = new osg::Image;
    crop->allocateImage(s, s, 1, GL_RGB, GL_UNSIGNED_BYTE);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x)
        {
            const unsigned char* src = img.data(left + x, top + y, 0);
            unsigned char* dst = crop->data(x, y, 0);
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    if (s != targetSize) crop->scaleImage(targetSize, targetSize, 1);

    const unsigned char* rz = crop->data();
    std::vector<float> out((size_t)3 * targetSize * targetSize);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < targetSize * targetSize; ++i)
            out[(size_t)c * targetSize * targetSize + i] = rz[i * 3 + c] / 255.0f;
    return out;
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------
FreeSplatter::GaussianContent FreeSplatter::estimateGaussians(const std::vector<osg::ref_ptr<osg::Image>>& inputs)
{
    FreeSplatterImpl* p = static_cast<FreeSplatterImpl*>(_internal.get());
    std::vector<float> buf; int sz = p->info.image_width;
    for (const auto& img : inputs)
    {
        auto chw = preprocessImage(*img, sz);
        buf.insert(buf.end(), chw.begin(), chw.end());
    }

    float* out = nullptr; size_t n_out = 0;
    GaussianContent g = {};
    if (free_splatter_run(p->ctx, buf.data(), (int)inputs.size(),
                          p->info.image_height, p->info.image_width, &out, &n_out) != 0)
    { FREESP_LOG_R(std::string("Run failed: ") + free_splatter_last_error(p->ctx), g); }

    g.raw.assign(out, out + n_out);
    g.n_views = (int)inputs.size();
    g.height  = p->info.image_height;
    g.width   = p->info.image_width;
    g.gaussian_channels = p->info.gaussian_channels;
    free_splatter_buf_free(out);
    return g;
}

FreeSplatter::GaussianContent FreeSplatter::estimateGaussians(osg::Image* input)
{
    std::vector<osg::ref_ptr<osg::Image>> v; v.push_back(input);
    return estimateGaussians(v);
}

// ---------------------------------------------------------------------------
// Raw dump I/O
// ---------------------------------------------------------------------------
FreeSplatter::GaussianContent FreeSplatter::loadFromRaw(const std::string& path,
                                                        int n_views, int height, int width, int gc)
{
    GaussianContent g = {};
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) FREESP_LOG_R("Cannot open raw file: " + path, g);

    size_t bytes = (size_t)f.tellg(); f.seekg(0, std::ios::beg);
    size_t expected = (size_t)n_views * height * width * gc * sizeof(float);
    if (bytes != expected) FREESP_LOG_R("Raw file size mismatch", g);

    g.raw.resize(bytes / sizeof(float));
    f.read((char*)g.raw.data(), bytes);
    g.n_views = n_views;
    g.height  = height;
    g.width   = width;
    g.gaussian_channels = gc;
    return g;
}

bool FreeSplatter::writeRaw(const GaussianContent& g, const std::string& path)
{
    std::ofstream f(path, std::ios::binary); if (!f) return false;
    f.write((const char*)g.raw.data(), g.raw.size() * sizeof(float));
    return f.good();
}

// ---------------------------------------------------------------------------
// Unpack to OSG arrays
// ---------------------------------------------------------------------------
void FreeSplatter::GaussianContent::unpack(float opacityThreshold)
{
    if (raw.empty()) return;
    size_t n = (size_t)n_views * height * width;
    positions  = new osg::Vec3Array;
    scales     = new osg::Vec3Array;
    rotations  = new osg::Vec4Array;
    colors     = new osg::Vec3Array;
    opacities  = new osg::FloatArray;
    positions->reserve(n); scales->reserve(n);
    rotations->reserve(n); colors->reserve(n);
    opacities->reserve(n);

    const double C0 = 0.28209479177387814;
    for (size_t i = 0; i < n; ++i)
    {
        const float* x = &raw[i * gaussian_channels];
        float op = x[15]; if (op <= opacityThreshold) continue;
        positions->push_back(osg::Vec3(x[0], x[1], x[2]));
        scales->push_back(osg::Vec3(x[16], x[17], x[18]));
        rotations->push_back(osg::Vec4(x[19], x[20], x[21], x[22])); // w,x,y,z
        colors->push_back(osg::Vec3(0.5f + (float)C0 * x[3],
                                    0.5f + (float)C0 * x[4],
                                    0.5f + (float)C0 * x[5]));
        opacities->push_back(op);
    }
}

// ---------------------------------------------------------------------------
// Pose recovery
// ---------------------------------------------------------------------------
std::vector<osg::Matrixf> FreeSplatter::estimatePoses(const GaussianContent& g,
                                                      float opacityThreshold, float* sharedFocal)
{
    std::vector<osg::Matrixf> mats;
    if (g.raw.empty()) FREESP_LOG_R("Empty gaussian content", mats);

    std::vector<float> c2w((size_t)g.n_views * 16); float focal = 0.0f;
    if (free_splatter_estimate_poses(g.raw.data(), g.n_views, g.height, g.width,
                                     g.gaussian_channels, opacityThreshold,
                                     c2w.data(), &focal) != 0)
    { FREESP_LOG_R("Pose estimation failed", mats); }

    if (sharedFocal) *sharedFocal = focal; mats.reserve(g.n_views);
    for (int i = 0; i < g.n_views; ++i)
    {
        const float* m = &c2w[(size_t)i * 16]; osg::Matrixf mat;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) mat(r, c) = m[r * 4 + c];
        mats.push_back(mat);
    }
    return mats;
}

// ---------------------------------------------------------------------------
// Parallax
// ---------------------------------------------------------------------------
FreeSplatter::ParallaxContent FreeSplatter::estimateParallax(const GaussianContent& g,
                                                             float opacityThreshold)
{
    ParallaxContent out = {};
    if (g.n_views < 2) FREESP_LOG_R("Parallax needs at least 2 views", out);

    free_splatter_parallax px;
    if (free_splatter_pair_parallax(g.raw.data(), g.n_views, g.height, g.width,
                                    g.gaussian_channels, opacityThreshold, &px) != 0)
    { FREESP_LOG_R("Parallax estimation failed", out); }

    out.tri_angle_deg     = px.tri_angle_deg;
    out.lateral_angle_deg = px.lateral_angle_deg;
    out.baseline_over_depth = px.baseline_over_depth;
    out.baseline          = px.baseline;
    out.median_depth      = px.median_depth;
    out.focal             = px.focal;
    out.n_points          = px.n_points;
    return out;
}

namespace
{
    // ---------------------------------------------------------------------------
    // AccumulatorImpl
    // ---------------------------------------------------------------------------
    class AccumulatorImpl : public osg::Referenced
    {
    public:
        free_splatter_accumulator* acc = nullptr;
        int width = 0, height = 0;

        AccumulatorImpl(int w, int h, float opac)
        {
            acc = free_splatter_accumulator_new(w, h, opac);
            if (!acc) FREESP_LOG("Accumulator alloc failed");
            width = w; height = h;
        }

        ~AccumulatorImpl()
        { if (acc) free_splatter_accumulator_free(acc); }
    };
}

// ---------------------------------------------------------------------------
// Accumulator
// ---------------------------------------------------------------------------
FreeSplatter::Accumulator::Accumulator(int width, int height, float opacityThreshold)
{ _internal = new AccumulatorImpl(width, height, opacityThreshold); }

FreeSplatter::Accumulator::~Accumulator()
{}

bool FreeSplatter::Accumulator::addPair(const GaussianContent& pairGaussians)
{
    if (pairGaussians.n_views != 2 || pairGaussians.raw.empty()) return false;
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    return free_splatter_accumulator_add_pair(p->acc, pairGaussians.raw.data(),
                                              pairGaussians.gaussian_channels) == 0;
}

int FreeSplatter::Accumulator::getFrameCount() const
{
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    return free_splatter_accumulator_frame_count(p->acc);
}

std::vector<FreeSplatter::Accumulator::Point> FreeSplatter::Accumulator::getCloud() const
{
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    free_splatter_point* cloud = nullptr; size_t n = 0;

    std::vector<Point> result;
    if (free_splatter_accumulator_cloud(p->acc, &cloud, &n) != 0 || !cloud)
        return result;

    result.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const free_splatter_point& pt = cloud[i];
        result.push_back({
            osg::Vec3f(pt.x, pt.y, pt.z),
            osg::Vec3f(pt.r, pt.g, pt.b), pt.opacity,
            osg::Vec3f(pt.sx, pt.sy, pt.sz),
            osg::Quat(pt.qx, pt.qy, pt.qz, pt.qw),
            pt.frame });
    }
    free_splatter_buf_free(cloud);
    return result;
}

std::vector<FreeSplatter::Accumulator::Point> FreeSplatter::Accumulator::fuse(
        float voxelFrac, int k, int mode) const
{
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    free_splatter_point* out = nullptr; size_t n_out = 0;

    std::vector<Point> result;
    if (free_splatter_accumulator_fuse(p->acc, voxelFrac, k, mode, &out, &n_out) != 0 || !out)
        return result;

    result.reserve(n_out);
    for (size_t i = 0; i < n_out; ++i)
    {
        const free_splatter_point& pt = out[i];
        result.push_back({
            osg::Vec3f(pt.x, pt.y, pt.z),
            osg::Vec3f(pt.r, pt.g, pt.b), pt.opacity,
            osg::Vec3f(pt.sx, pt.sy, pt.sz),
            osg::Quat(pt.qx, pt.qy, pt.qz, pt.qw),
            pt.frame });
    }
    free_splatter_buf_free(out);
    return result;
}

bool FreeSplatter::Accumulator::refine(float voxelFrac, int iters, float alpha)
{
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    return free_splatter_accumulator_refine(p->acc, voxelFrac, iters, alpha) == 0;
}

std::vector<osg::Matrixf> FreeSplatter::Accumulator::getCameraPath() const
{
    AccumulatorImpl* p = static_cast<AccumulatorImpl*>(_internal.get());
    float* out = nullptr; int n_frames = 0;

    std::vector<osg::Matrixf> mats;
    if (free_splatter_accumulator_camera_path(p->acc, &out, &n_frames) != 0 || !out)
        return mats;

    for (int i = 0; i < n_frames; ++i)
    {
        const float* m = &out[(size_t)i * 16]; osg::Matrixf mat;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) mat(r, c) = m[r * 4 + c];
        mats.push_back(mat);
    }
    free_splatter_buf_free(out);
    return mats;
}

// ---------------------------------------------------------------------------
// Tree overlap
// ---------------------------------------------------------------------------
std::vector<FreeSplatter::Accumulator::Point> FreeSplatter::treeOverlap(
        const std::vector<GaussianContent>& pairs,
        int width, int height, float opacityThreshold, const TreeOptions& opts)
{
    if (pairs.empty()) return {};
    int gc = pairs[0].gaussian_channels;
    std::vector<const float*> ptrs;
    ptrs.reserve(pairs.size());
    for (const auto& g : pairs)
        ptrs.push_back(g.raw.data());

    free_splatter_point* out = nullptr;
    size_t n_out = 0; int n_nodes = 0;
    if (free_splatter_tree_overlap(ptrs.data(), (int)ptrs.size(), gc,
                                   height, width, opacityThreshold,
                                   opts.block, opts.overlap, opts.maxLevels,
                                   opts.layoutSpacing, opts.perNodeCap,
                                   &out, &n_out, &n_nodes) != 0)
    { FREESP_LOG_R("tree overlap failed", {}); }

    std::vector<Accumulator::Point> result;
    if (out)
    {
        result.reserve(n_out);
        for (size_t i = 0; i < n_out; ++i)
        {
            const free_splatter_point& pt = out[i];
            result.push_back({
                osg::Vec3f(pt.x, pt.y, pt.z),
                osg::Vec3f(pt.r, pt.g, pt.b), pt.opacity,
                osg::Vec3f(pt.sx, pt.sy, pt.sz),
                osg::Quat(pt.qx, pt.qy, pt.qz, pt.qw),
                pt.frame });
        }
        free_splatter_buf_free(out);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Splat export (accumulated cloud)
// ---------------------------------------------------------------------------
bool FreeSplatter::writeSplat(const std::vector<Accumulator::Point>& points,
                              const std::string& path, size_t maxSplats, float scaleMult)
{
    auto importance = [&](size_t i) -> double {
        const Accumulator::Point& p = points[i];
        double vol = (double)osg::maximum(p.scale.x(), 1e-9f) *
                     osg::maximum(p.scale.y(), 1e-9f) *
                     osg::maximum(p.scale.z(), 1e-9f);
        return (double)osg::maximum(p.opacity, 0.0f) * vol;
    };

    std::vector<size_t> idx(points.size());
    std::iota(idx.begin(), idx.end(), 0);
    if (maxSplats > 0 && points.size() > maxSplats)
    {
        std::partial_sort(idx.begin(), idx.begin() + maxSplats, idx.end(),
            [&](size_t a, size_t b) { return importance(a) > importance(b); });
        idx.resize(maxSplats);
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    for (size_t i : idx)
    {
        const Accumulator::Point& p = points[i];
        float pos[3]   = { p.position.x(), p.position.y(), p.position.z() };
        float scale[3] = { scaleMult * p.scale.x(), scaleMult * p.scale.y(), scaleMult * p.scale.z() };
        float quat[4]  = { (float)p.rotation.w(), (float)p.rotation.x(),
                           (float)p.rotation.y(), (float)p.rotation.z() };
        float rgb[3]   = { p.color.x(), p.color.y(), p.color.z() };

        unsigned char rec[32];
        free_splatter::encode_splat_record(rec, pos, scale, quat, rgb, p.opacity);
        f.write((const char*)rec, 32);
    }
    return f.good();
}

// ---------------------------------------------------------------------------
// Splat export (single-run GaussianContent)
// ---------------------------------------------------------------------------
bool FreeSplatter::writeSplat(const GaussianContent& g, const std::string& path,
                              float opacityThreshold, size_t maxSplats)
{
    size_t n = (size_t)g.n_views * g.height * g.width;
    std::vector<std::pair<float, size_t>> keep; keep.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const float* x = &g.raw[i * g.gaussian_channels];
        float op = x[15]; if (op <= opacityThreshold) continue;
        float vol = std::max(x[16], 1e-9f) * std::max(x[17], 1e-9f) * std::max(x[18], 1e-9f);
        keep.push_back({ op * vol, i });
    }

    std::sort(keep.begin(), keep.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t m = (maxSplats > 0) ? std::min(maxSplats, keep.size()) : keep.size();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const double C0 = 0.28209479177387814;
    for (size_t k = 0; k < m; ++k)
    {
        const float* x = &g.raw[keep[k].second * g.gaussian_channels];
        float pos[3]   = { x[0], x[1], x[2] };
        float scale[3] = { x[16], x[17], x[18] };
        float quat[4]  = { x[19], x[20], x[21], x[22] };
        float rgb[3]   = { 0.5f + (float)C0 * x[3],
                           0.5f + (float)C0 * x[4],
                           0.5f + (float)C0 * x[5] };
        unsigned char rec[32];
        free_splatter::encode_splat_record(rec, pos, scale, quat, rgb, x[15]);
        f.write((const char*)rec, 32);
    }
    return f.good();
}
