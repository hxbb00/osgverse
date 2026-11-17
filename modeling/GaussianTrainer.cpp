#include "GaussianTrainer.h"
#include "gs_trainer.h"
#include "perf_timer.h"
#include "config.h"

#include <osg/Array>
#include <osg/Math>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>

namespace osgVerse
{

struct GaussianTrainer::Impl
{
    VulkanGSTrainer trainer;
    VulkanGSPipelineBuffers buffers;
    VulkanGSRendererUniforms uniforms;
    TrainerConfig config;
    GaussianTrainerParameters params;
    std::vector<size_t> shuffle;
    std::mt19937 rng;
    int step = 0;
    bool initialized = false;
    bool ready = false;
};

namespace
{

VkSplatDataset::Camera convertCamera(const TrainerInput::Camera& c)
{
    VkSplatDataset::Camera vc;
    vc.model = (VkSplatDataset::Camera::Model)c.model;
    vc.h = c.height; vc.w = c.width;
    vc.fx = c.fx; vc.fy = c.fy; vc.cx = c.cx; vc.cy = c.cy;
    vc.k1 = c.k1; vc.k2 = c.k2; vc.p1 = c.p1; vc.p2 = c.p2;
    vc.k3 = c.k3; vc.k4 = c.k4; vc.k5 = c.k5; vc.k6 = c.k6;
    vc.sx1 = c.sx1; vc.sy1 = c.sy1;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            vc.camtoworld(i, j) = c.camToWorld(i, j);
    return vc;
}

std::string ensureTrailingSlash(std::string dir)
{
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

}  // namespace

GaussianTrainer::GaussianTrainer() : _impl(new Impl)
{
}

GaussianTrainer::~GaussianTrainer()
{
    if (_impl)
    {
        if (_impl->initialized)
        {
            try
            {
                _impl->trainer.cleanupBuffers(_impl->buffers);
                _impl->trainer.cleanup();
            }
            catch (...) {}
        }
        delete _impl; _impl = NULL;
    }
}

bool GaussianTrainer::initialize(const TrainerInput* input,
                                 const GaussianTrainerParameters& params)
{
    if (!_impl) _impl = new Impl;
    Impl& d = *_impl;
    _lastError.clear(); d.ready = false;
    if (!input)
    {
        _lastError = "TrainerInput is null";
        return false;
    }
    if (params.shaderDir.empty())
    {
        _lastError = "shaderDir is empty (expected .../shaders/3dgs_trainer/)";
        return false;
    }

    try
    {
        if (d.initialized)
        {
            d.trainer.cleanupBuffers(d.buffers);
            d.trainer.cleanup();
            d.buffers = VulkanGSPipelineBuffers();
            d.initialized = false;
        }

        // Shader paths: SPIR-V files under generated/ and radix_sort/
        const std::string dir = ensureTrailingSlash(params.shaderDir);
        const char* names[] = {
            "projection_forward", "generate_keys", "compute_tile_ranges",
            "rasterize_forward", "rasterize_backward_0", "rasterize_backward_1",
            "rasterize_backward_2", "rasterize_backward_3", "rasterize_backward_4",
            "cumsum_single_pass", "cumsum_block_scan", "cumsum_scan_block_sums",
            "cumsum_add_block_offsets", "radix_sort/upsweep", "radix_sort/spine",
            "radix_sort/downsweep", "ssim_forward", "ssim_backward",
            "fused_projection_backward_optimizer", "sum", "where",
            "default_update_state", "default_compute_grow_mask", "default_duplicate",
            "default_split", "default_compute_prune_mask", "default_prune",
            "default_prune_mean", "default_prune_sh", "default_reset_opa",
            "mcmc_inject_noise", "mcmc_compute_probs",
            "mcmc_compute_relocation_index_map", "mcmc_compute_relocation",
            "mcmc_update_relocation", "mcmc_compute_add_index_map", "mcmc_compute_add",
            "mcmc_update_add", "morton_sort_compute_stats", "morton_sort_generate_keys",
            "morton_sort_apply_indices", "morton_sort_apply_indices_sh",
            "morton_sort_update_buffer", "morton_sort_update_buffer_sh",
            NULL
        };
        std::map<std::string, std::string> shaderPaths;
        for (int i = 0; names[i] != NULL; ++i)
        {
            const std::string name(names[i]);
            bool isRadix = (name.find('/') != std::string::npos);
            shaderPaths[name] = dir + (isRadix ? (name + ".spv") : ("generated/" + name + ".spv"));
        }

        d.trainer.initialize(shaderPaths, params.deviceId);
        d.initialized = true;
        PerfTimer::reset();

        /* Convert TrainerInput to the neutral Vulkan dataset */
        VkSplatDataset dataset;
        dataset.train.reserve(input->trainCameras.size());
        dataset.val.reserve(input->valCameras.size());
        for (size_t i = 0; i < input->trainCameras.size(); ++i)
        {
            const TrainerInput::Camera& c = input->trainCameras[i];
            VkSplatDataset::Image im;
            im.image_path = c.imagePath; im.mask_path = c.maskPath;
            im.camera = convertCamera(c);
            dataset.train.push_back(im);
        }
        for (size_t i = 0; i < input->valCameras.size(); ++i)
        {
            const TrainerInput::Camera& c = input->valCameras[i];
            VkSplatDataset::Image im;
            im.image_path = c.imagePath; im.mask_path = c.maskPath;
            im.camera = convertCamera(c);
            dataset.val.push_back(im);
        }
        dataset.points.resize(input->points.size());
        dataset.point_colors.resize(input->pointColors.size());
        for (size_t i = 0; i < input->points.size(); ++i)
            dataset.points[i] = Eigen::Vector3f(input->points[i].x(), input->points[i].y(),
                                                input->points[i].z());
        for (size_t i = 0; i < input->pointColors.size(); ++i)
            dataset.point_colors[i] = Eigen::Vector3f(input->pointColors[i].x(),
                                                      input->pointColors[i].y(),
                                                      input->pointColors[i].z());
        dataset.global_scale = params.globalScale;
        if (dataset.points.empty() || dataset.points.size() != dataset.point_colors.size())
        {
            _lastError = "Invalid sparse point cloud (points/colors empty or mismatched)";
            return false;
        }

        /* Translate parameters to VkSplat's TrainerConfig */
        TrainerConfig& cfg = d.config;
        cfg.output_dir = params.outputDir;
        cfg.output_ply = "";
        cfg.dataset_dir = ""; cfg.image_dir = ""; cfg.mask_dir = ""; cfg.sparse_dir = "";
        cfg.eval_interval = input->evalInterval;
        cfg.image_cache_device = params.cacheImagesOnGPU
                               ? TrainerConfig::CacheImage::GPU : TrainerConfig::CacheImage::CPU;
        cfg.global_scale = params.globalScale;
        cfg.init_scale = params.initScale;
        cfg.init_opacity = params.initOpacity;
        cfg.strategy = params.useMCMC ? TrainerConfig::Strategy::MCMC
                                      : TrainerConfig::Strategy::Default;
        cfg.max_steps = params.maxSteps;
        cfg.ssim_lambda = params.ssimLambda;
        cfg.means_lr = params.meansLr;
        cfg.means_lr_final = params.meansLrFinal;
        cfg.features_dc_lr = params.featuresDcLr;
        cfg.features_rest_lr = params.featuresRestLr;
        cfg.opacities_lr = params.opacitiesLr;
        cfg.scales_lr = params.scalesLr;
        cfg.quats_lr = params.quatsLr;
        cfg.scale_reg = params.scaleReg;
        cfg.opacity_reg = params.opacityReg;
        cfg.refine_start_iter = params.refineStartIter;
        cfg.refine_stop_iter = params.refineStopIter;
        cfg.refine_every = params.refineEvery;
        cfg.prune_opa = params.pruneOpa;
        cfg.grow_grad2d = params.growGrad2d;
        cfg.grow_scale3d = params.growScale3d;
        cfg.grow_scale2d = params.growScale2d;
        cfg.prune_scale3d = params.pruneScale3d;
        cfg.prune_scale2d = params.pruneScale2d;
        cfg.refine_scale2d_stop_iter = params.refineScale2dStopIter;
        cfg.reset_every = params.resetEvery;
        cfg.stop_reset_at = params.stopResetAt;
        cfg.pause_refine_after_reset = params.pauseRefineAfterReset;
        cfg.noise_lr = params.noiseLr;
        cfg.min_opacity = params.minOpacity;
        cfg.grow_factor = params.growFactor;
        cfg.cap_max = params.capMax;

        // Standard MCMC overrides, matching simple_trainer.py's MCMCTrainerConfig
        if (params.useMCMC)
        {
            if (cfg.init_scale == 1.0f) cfg.init_scale = 0.1f;
            if (cfg.init_opacity == 0.1f) cfg.init_opacity = 0.5f;
            if (cfg.scale_reg == 0.0f) cfg.scale_reg = 0.01f;
            if (cfg.opacity_reg == 0.0f) cfg.opacity_reg = 0.01f;
            if (cfg.refine_stop_iter == 15000) cfg.refine_stop_iter = 25000;
        }

        d.params = params;
        d.trainer.loadDataset(dataset, cfg, d.buffers);
        d.trainer.get_train_camera(0, d.uniforms);
        d.uniforms.active_sh = 3;
        d.uniforms.step = 0;

        d.step = 0;
        d.shuffle.resize(d.trainer.num_train());
        std::iota(d.shuffle.begin(), d.shuffle.end(), (size_t)0);
        d.rng.seed(42);
        std::shuffle(d.shuffle.begin(), d.shuffle.end(), d.rng);
        d.ready = true;
        return true;
    }
    catch (const std::exception& e)
    {
        _lastError = e.what();
        d.ready = false;
        return false;
    }
}

bool GaussianTrainer::step()
{
    if (!_impl || !_impl->ready) return false;
    Impl& d = *_impl;
    if (d.step >= d.config.max_steps) return false;

    const size_t numTrain = d.trainer.num_train();
    if (numTrain == 0) { _lastError = "No training image"; return false; }
    if (d.step > 0 && (d.step % numTrain) == 0)
        std::shuffle(d.shuffle.begin(), d.shuffle.end(), d.rng);

    const size_t imageIdx = d.shuffle[d.step % numTrain];
    try
    {
        d.trainer.get_train_camera(imageIdx, d.uniforms);
        d.uniforms.active_sh = std::min<uint32_t>((uint32_t)(d.step / 1000), 3u);
        d.uniforms.step = (uint32_t)d.step;
        d.uniforms.num_splats = (uint32_t)d.buffers.num_splats;

        auto deviceGuard = DeviceGuard(&d.trainer);
        const size_t allocReserve =
            (d.config.strategy == TrainerConfig::Strategy::MCMC) ? (size_t)d.config.cap_max : 0;

        // Forward: projection, tiling/sorting, rasterization
        d.trainer.executeProjectionForward(d.uniforms, d.buffers, allocReserve);
        d.trainer.executeCalculateIndexBufferOffset(d.buffers);
        if (d.buffers.num_indices > 0)
        {
            d.trainer.executeGenerateKeys(d.uniforms, d.buffers);
            d.trainer.executeSort(d.uniforms, d.buffers, -1);
            d.trainer.executeComputeTileRanges(d.uniforms, d.buffers);
        }
        d.trainer.executeRasterizeForward(d.uniforms, d.buffers);

        // Loss gradient, rasterization backward, fused projection backward + Adam
        if (d.buffers.num_indices > 0)
            d.trainer.executeComputeSSIMGradient(d.config, d.uniforms, d.buffers, imageIdx);
        d.trainer.executeRasterizeBackward(d.uniforms, d.buffers);
        d.trainer.executeFusedProjectionBackwardOptimizerStep(
            d.config, d.uniforms, d.buffers, d.step + 1);

        // Densification
        if (d.config.strategy == TrainerConfig::Strategy::Default)
            d.trainer.executeDefaultPostBackward(d.config, d.uniforms, d.buffers, d.step);
        else
            d.trainer.executeMCMCPostBackward(d.config, d.uniforms, d.buffers, d.step);

        ++d.step;
        return true;
    }
    catch (const std::exception& e)
    {
        _lastError = e.what();
        d.ready = false;
        return false;
    }
}

bool GaussianTrainer::train()
{
    if (!_impl || !_impl->ready) return false;
    while (_impl->step < _impl->config.max_steps)
    {
        if (!step()) return false;
    }
    return true;
}

bool GaussianTrainer::savePLY(const std::string& fileName)
{
    if (!_impl || !_impl->initialized) { _lastError = "Trainer not initialized"; return false; }
    try
    {
        _impl->trainer.writePLY(fileName, _impl->buffers);
        return true;
    }
    catch (const std::exception& e)
    {
        _lastError = e.what();
        return false;
    }
}

osg::ref_ptr<GaussianGeometry> GaussianTrainer::createGeometry()
{
    if (!_impl || !_impl->initialized || _impl->buffers.num_splats == 0)
        return NULL;

    Impl& d = *_impl;
    const size_t n = d.buffers.num_splats;

    // One-time readback. Training itself never touches OpenGL.
    Buffer<float> xyz, rot, so, sh;
    xyz.deviceBuffer = d.buffers.xyz_ws.deviceBuffer;
    rot.deviceBuffer = d.buffers.rotations.deviceBuffer;
    so.deviceBuffer = d.buffers.scales_opacs.deviceBuffer;
    sh.deviceBuffer = d.buffers.sh_coeffs.deviceBuffer;
    try
    {
        d.trainer.copyFromDevice(xyz);
        d.trainer.copyFromDevice(rot);
        d.trainer.copyFromDevice(so);
        d.trainer.copyFromDevice(sh);
    }
    catch (const std::exception& e)
    {
        _lastError = e.what();
        return NULL;
    }

    osg::ref_ptr<osg::Vec3Array> pos = new osg::Vec3Array; pos->resize(n);
    osg::ref_ptr<osg::Vec3Array> scale = new osg::Vec3Array; scale->resize(n);
    osg::ref_ptr<osg::Vec4Array> quat = new osg::Vec4Array; quat->resize(n);
    osg::ref_ptr<osg::FloatArray> alpha = new osg::FloatArray; alpha->resize(n);
    osg::ref_ptr<osg::Vec4Array> shR[4], shG[4], shB[4];
    for (int i = 0; i < 4; ++i)
    {
        shR[i] = new osg::Vec4Array; shR[i]->resize(n);
        shG[i] = new osg::Vec4Array; shG[i]->resize(n);
        shB[i] = new osg::Vec4Array; shB[i]->resize(n);
    }

    // SH is stored in the subgroup-aligned layout inside the buffer. Instead of
    // converting the whole array in place, map each coefficient on the fly.
    static const size_t SH_DIM = 12;
    auto forwardIndex = [](size_t i) {
        size_t groupIdx = i / (SH_DIM * SH_REORDER_SIZE);
        size_t gaussIdx = (i / SH_DIM) % SH_REORDER_SIZE;
        size_t shIdx = i % SH_DIM;
        return (groupIdx * SH_DIM + shIdx) * SH_REORDER_SIZE + gaussIdx;
    };

    for (size_t g = 0; g < n; ++g)
    {
        (*pos)[g].set(xyz[3*g+0], xyz[3*g+1], xyz[3*g+2]);
        (*scale)[g].set(so[4*g+0], so[4*g+1], so[4*g+2]);
        (*alpha)[g] = osg::clampBetween(so[4*g+3], 0.0f, 1.0f);
        // VkSplat stores (w, x, y, z); GaussianGeometry expects (x, y, z, w)
        (*quat)[g].set(rot[4*g+1], rot[4*g+2], rot[4*g+3], rot[4*g+0]);

        size_t reordered[SH_DIM];
        for (size_t u = 0; u < SH_DIM; ++u)
            reordered[u] = forwardIndex(g * SH_DIM + u);

        for (size_t c = 0; c < 16; ++c)
        {
            size_t group = c / 4, slot = c % 4;
            float values[3];
            for (size_t ch = 0; ch < 3; ++ch)
            {
                size_t f = 3 * c + ch;
                values[ch] = sh[4 * reordered[f / 4] + (f % 4)];
            }
            (*shR[group])[g][slot] = values[0];
            (*shG[group])[g][slot] = values[1];
            (*shB[group])[g][slot] = values[2];
        }
    }

    osg::ref_ptr<GaussianGeometry> geom =
        new GaussianGeometry(GaussianGeometry::INSTANCING);
    geom->setShDegrees(3);
    geom->setPosition(pos.get());
    geom->setScaleAndRotation(scale.get(), quat.get(), alpha.get());
    for (int i = 0; i < 4; ++i)
    {
        geom->setShRed(i, shR[i].get());
        geom->setShGreen(i, shG[i].get());
        geom->setShBlue(i, shB[i].get());
    }
    geom->finalize();
    return geom;
}

int GaussianTrainer::getCurrentStep() const { return _impl ? _impl->step : 0; }

int GaussianTrainer::getNumSplats() const
{
    return (_impl && _impl->initialized) ? (int)_impl->buffers.num_splats : 0;
}

size_t GaussianTrainer::getVRAMUsage() const
{
    return (_impl && _impl->initialized) ? _impl->buffers.getTotalAllocSize() : 0;
}

size_t GaussianTrainer::getPeakVRAMUsage() const
{
    return (_impl && _impl->initialized) ? _impl->trainer.getPeakAllocSize() : 0;
}

osg::Matrixf GaussianTrainer::getDataparserTransform() const
{
    osg::Matrixf result = osg::Matrixf::identity();
    if (!_impl || !_impl->initialized) return result;
    Eigen::Matrix4f transform = _impl->trainer.get_dataparser_transform();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            result(i, j) = transform(i, j);
    return result;
}

std::map<std::string, std::tuple<size_t, double>>
GaussianTrainer::getTimingBreakdown() const
{
    return PerfTimer::get_summary();
}

}  // namespace osgVerse
