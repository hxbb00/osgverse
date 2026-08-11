#include <depthanything3/engine.hpp>
#include <depthanything3/quantize.hpp>
#include <depthanything3/preprocess.hpp>
#include <depthanything3/colmap_export.hpp>
#include "DepthAnythingEngine.h"
#include <limits.h>

using namespace osgVerse;
#define DA3_LOG(m) OSG_NOTICE << "[DepthAnything] " << m;

namespace
{
    struct DaEngineHandle : public osg::Referenced
    { std::unique_ptr<da::Engine> engine; };

    bool convertDaImage(const osg::Image& image, da::Image& out)
    {
        if (!image.valid()) return false;
        if (image.getDataType() != GL_UNSIGNED_BYTE) return false;
        if (image.getPixelFormat() != GL_RGB) return false;
        out.h = image.t(); out.w = image.s(); out.rgb.resize(image.getTotalSizeInBytes());
        memcpy(out.rgb.data(), image.data(), image.getTotalSizeInBytes()); return true;
    }

    osg::Image* convertImage(const std::vector<float>& buffer, int w, int h, bool normalizedResult)
    {
        osg::ref_ptr<osg::Image> img = new osg::Image;
        if (normalizedResult)
        {
            float minV = FLT_MAX, maxV = 0.0f, invRange = 0.0f;
            for (const float& v : buffer) { if (v < minV) minV = v; if (v > maxV) maxV = v; }
            invRange = maxV - minV; if (invRange <= 0.0f) return NULL; else invRange = 1.0f / invRange;

            std::vector<unsigned char> buffer0(buffer.size());
#pragma omp parallel for
            for (int i = 0; i < (int)buffer.size(); ++i)
            {
                float v = 1.0f - ((buffer[i] - minV) * invRange);
                buffer0[i] = (unsigned char)(v * 255.0f + 0.5f);
            }
            img->allocateImage(w, h, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);
            img->setInternalTextureFormat(GL_LUMINANCE8);
            memcpy(img->data(), buffer0.data(), buffer0.size());
        }
        else
        {
            img->allocateImage(w, h, 1, GL_RED, GL_FLOAT);
            img->setInternalTextureFormat(GL_R32F);
            memcpy(img->data(), buffer.data(), buffer.size());
        }
        return img.release();
    }

    bool convertGaussians(da::Gaussians& in, DepthAnything::GaussianContent& out)
    {
        if (in.N <= 0) return false;
        out.positions = new osg::Vec3Array(in.N); out.scales = new osg::Vec3Array(in.N);
        out.rotations = new osg::Vec4Array(in.N); out.alphas = new osg::FloatArray(in.N);
        out.reds = new osg::Vec4Array(in.N); out.greens = new osg::Vec4Array(in.N); out.blues = new osg::Vec4Array(in.N);

        for (int i = 0; i < in.N; ++i)
        {
            int idx0 = i * 3, idx1 = i * 4;
            (*out.positions)[i] = osg::Vec3(in.means[idx0 + 0], in.means[idx0 + 1], in.means[idx0 + 2]);
            (*out.scales)[i] = osg::Vec3(osg::maximum(1e-6f, in.scales[idx0 + 0]),
                                         osg::maximum(1e-6f, in.scales[idx0 + 1]),
                                         osg::maximum(1e-6f, in.scales[idx0 + 2]));
            (*out.rotations)[i] = osg::Vec4(in.rotations[idx1 + 1], in.rotations[idx1 + 2],
                                            in.rotations[idx1 + 3], in.rotations[idx1 + 0]);
            (*out.alphas)[i] = osg::minimum(1.0f - 1e-6f, osg::maximum(1e-6f, in.opacities[i]));
            if ((idx0 + 2) < in.colors.size())
            {
                (*out.reds)[i] = osg::Vec4(in.colors[idx0 + 0], 0.0f, 0.0f, 0.0f);
                (*out.greens)[i] = osg::Vec4(in.colors[idx0 + 1], 0.0f, 0.0f, 0.0f);
                (*out.blues)[i] = osg::Vec4(in.colors[idx0 + 2], 0.0f, 0.0f, 0.0f);
            }
            else
            {
                (*out.reds)[i] = osg::Vec4(in.harmonics[(idx0 + 0) * 9], in.harmonics[(idx0 + 0) * 9 + 1],
                                           in.harmonics[(idx0 + 0) * 9 + 2], in.harmonics[(idx0 + 0) * 9 + 3]);
                (*out.greens)[i] = osg::Vec4(in.harmonics[(idx0 + 1) * 9], in.harmonics[(idx0 + 1) * 9 + 1],
                                             in.harmonics[(idx0 + 1) * 9 + 2], in.harmonics[(idx0 + 1) * 9 + 3]);
                (*out.blues)[i] = osg::Vec4(in.harmonics[(idx0 + 2) * 9], in.harmonics[(idx0 + 2) * 9 + 1],
                                            in.harmonics[(idx0 + 2) * 9 + 2], in.harmonics[(idx0 + 2) * 9 + 3]);
            }
        }
        return true;
    }
}

DepthAnything::DepthAnything(const std::string& model, int numThreads)
{
    DaEngineHandle* da = new DaEngineHandle; _internal = da;
    da->engine = da::Engine::load(model, numThreads);
}

DepthAnything::DepthAnything(const std::string& model, const std::string& metricModel, int numThreads)
{
    DaEngineHandle* da = new DaEngineHandle; _internal = da;
    da->engine = da::Engine::load_nested(model, metricModel, numThreads);
}

DepthAnything::~DepthAnything()
{
}

bool DepthAnything::checkModelInfomation(ModelInformation& info)
{
    da::Engine* engine = _internal.valid() ? static_cast<DaEngineHandle*>(_internal.get())->engine.get() : NULL;
    if (!engine) { DA3_LOG("Failed to load model\n"); return false; }

    const auto& cfg = engine->config();
    info.checkpoint = cfg.checkpoint_name; info.architecture = cfg.arch;
    info.ffn_type = cfg.ffn_type; info.image_resize_mode = cfg.img_resize_mode;
    info.img_resize_target = cfg.img_resize_target; info.patch_size = cfg.patch_size;
    info.depth = cfg.depth; info.embed_dim = cfg.embed_dim; info.head_dim = cfg.head_dim;
    info.mlp_hidden = cfg.mlp_hidden; info.num_heads = cfg.num_heads; return true;
}

DepthAnything::DepthContent DepthAnything::estimateDepth(const osg::Image& input, bool outputPose, bool normalizedResult)
{
    DepthAnything::DepthContent depth = {}; da::Image image;
    if (!convertDaImage(input, image)) { DA3_LOG("Input image format must be RGB8\n"); return depth; }

    da::Engine* engine = _internal.valid() ? static_cast<DaEngineHandle*>(_internal.get())->engine.get() : NULL;
    if (!engine) { DA3_LOG("Failed to load model\n"); return depth; }
    else if (engine->is_nested()) { DA3_LOG("Current model is metric\n"); return depth; }

    if (engine->is_da2())  // DepthAnything V2: relative depth only
    {
        std::vector<float> out; int H = 0, W = 0;
        if (!engine->depth_relative(image, out, H, W)) { DA3_LOG("DA2 depth estimating failed\n"); return depth; }
        depth.depth = convertImage(out, W, H, normalizedResult);
    }
    else if (engine->is_mono())  // DA3MONO checkpoint (output_dim==1 + sky_head): depth + sky
    {
        std::vector<float> out, sky; int H = 0, W = 0;
        if (!engine->depth_mono(image, out, sky, H, W)) { DA3_LOG("Mono depth estimating failed\n"); return depth; }
        depth.depth = convertImage(out, W, H, normalizedResult);
        depth.mono_sky = convertImage(sky, W, H, false);
    }
    else  // Default DA3: native-resolution real DA3 resize
    {
        std::vector<float> out, conf; int H = 0, W = 0; bool ok = false;
        if (outputPose)
        {
            std::array<float, 12> ext; std::array<float, 9> intr;
            if (engine->has_aux())  // Requires an aux-bearing GGUF
                ok = engine->depth_pose_rays_native(image, out, conf, ext, intr, H, W);
            else
                ok = engine->depth_pose_native(image, out, conf, ext, intr, H, W);
            
            if (ok)
            {
                depth.extrinsics.set(ext[0], ext[1], ext[2], 0.0f, ext[3], ext[4], ext[5], 0.0f,
                                     ext[6], ext[7], ext[8], 0.0f, ext[9], ext[10], ext[11], 1.0f);
                depth.intrinsics.set(intr[0], intr[1], intr[2], 0.0f, intr[3], intr[4], intr[5], 0.0f,
                                     intr[6], intr[7], intr[8], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
        else
            ok = engine->depth_native_image(image, out, conf, H, W);
        
        if (ok)
        {
            depth.depth = convertImage(out, W, H, normalizedResult);
            depth.confidence = convertImage(conf, W, H, false);
        }
        else
            { DA3_LOG("DA3 depth estimating failed\n"); }
    }
    return depth;
}

DepthAnything::DepthContent DepthAnything::estimateDepthMetric(const osg::Image& input, bool normalizedResult)
{
    DepthAnything::DepthContent depth = {}; da::Image image;
    if (!convertDaImage(input, image)) { DA3_LOG("Input image format must be RGB8\n"); return depth; }

    da::Engine* engine = _internal.valid() ? static_cast<DaEngineHandle*>(_internal.get())->engine.get() : NULL;
    if (!engine) { DA3_LOG("Failed to load metric model\n"); return depth; }
    else if (!engine->is_nested()) { DA3_LOG("Current model is not metric\n"); return depth; }

    da::NestedOut out; int H = 0, W = 0;
    if (engine->depth_metric(image, out, H, W))
    {
        const std::array<float, 12>& ext = out.extrinsics;
        const std::array<float, 9>& intr = out.intrinsics;
        depth.extrinsics.set(ext[0], ext[1], ext[2], 0.0f, ext[3], ext[4], ext[5], 0.0f,
                             ext[6], ext[7], ext[8], 0.0f, ext[9], ext[10], ext[11], 1.0f);
        depth.intrinsics.set(intr[0], intr[1], intr[2], 0.0f, intr[3], intr[4], intr[5], 0.0f,
                             intr[6], intr[7], intr[8], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        depth.depth = convertImage(out.depth, W, H, normalizedResult);
    }
    else
        { DA3_LOG("DA3 metric depth estimating failed\n"); }
    return depth;
}

std::vector<DepthAnything::DepthContent> DepthAnything::estimateDepths(const std::vector<osg::Image>& inputs,
                                                                       bool normalizedResult)
{
    std::vector<da::Image> images;
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        da::Image img = {};
        if (convertDaImage(inputs[i], img)) images.push_back(img);
    }

    std::vector<DepthAnything::DepthContent> depths;
    if (images.empty()) { DA3_LOG("No valid inputs. Input image format must be RGB8\n"); return depths; }

    da::Engine* engine = _internal.valid() ? static_cast<DaEngineHandle*>(_internal.get())->engine.get() : NULL;
    if (!engine) { DA3_LOG("Failed to load model\n"); return depths; }
    else if (engine->is_nested()) { DA3_LOG("Current model is metric\n"); return depths; }
    
    std::vector<da::ViewResult> views; int H = 0, W = 0;
    if (engine->depth_pose_multi(images, views, H, W))
    {
        depths.resize(views.size());
        for (size_t i = 0; i < views.size(); ++i)
        {
            const auto& r = views[i]; DepthAnything::DepthContent& depth = depths[i];
            depth.extrinsics.set(r.ext[0], r.ext[1], r.ext[2], 0.0f, r.ext[3], r.ext[4], r.ext[5], 0.0f,
                                 r.ext[6], r.ext[7], r.ext[8], 0.0f, r.ext[9], r.ext[10], r.ext[11], 1.0f);
            depth.intrinsics.set(r.intr[0], r.intr[1], r.intr[2], 0.0f, r.intr[3], r.intr[4], r.intr[5], 0.0f,
                                 r.intr[6], r.intr[7], r.intr[8], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            depth.depth = convertImage(r.depth, W, H, normalizedResult);
            depth.confidence = convertImage(r.conf, W, H, false);
        }
    }
    else
        { DA3_LOG("DA3 multi-depth estimating failed\n"); }
    return depths;
}

DepthAnything::GaussianContent DepthAnything::reconstruct(const osg::Image& input, DepthResultFunc func)
{
    DepthAnything::GaussianContent gs = {}; da::Image image;
    if (!convertDaImage(input, image)) { DA3_LOG("Input image format must be RGB8\n"); return gs; }

    da::Engine* engine = _internal.valid() ? static_cast<DaEngineHandle*>(_internal.get())->engine.get() : NULL;
    if (!engine) { DA3_LOG("Failed to load model\n"); return gs; }
    else if (engine->is_nested()) { DA3_LOG("Current model is metric\n"); return gs; }

    da::Gaussians out; int H = 0, W = 0;
    bool ok = (func != NULL) ? engine->reconstruct_ex(image, out, H, W, func)
                             : engine->reconstruct(image, out, H, W);
    if (ok)
    {
        convertGaussians(out, gs);
        gs.extrinsics.set(out.ext[0], out.ext[1], out.ext[2], 0.0f, out.ext[3], out.ext[4], out.ext[5], 0.0f,
                          out.ext[6], out.ext[7], out.ext[8], 0.0f, out.ext[9], out.ext[10], out.ext[11], 1.0f);
        gs.intrinsics.set(out.intr[0], out.intr[1], out.intr[2], 0.0f, out.intr[3], out.intr[4], out.intr[5], 0.0f,
                          out.intr[6], out.intr[7], out.intr[8], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    else
        { DA3_LOG("DA3 gaussian reconstruction failed\n"); }
    return gs;
}
