// Vendored from VkSplat (https://github.com/harry7557558/vksplat), Apache-2.0.
// Modified by osgVerse:
//   - glm replaced with Eigen
//   - COLMAP loading replaced by loadDataset(VkSplatDataset)
#include "gs_trainer.h"

#include "knn.h"
#include "normalize.h"

#include <Eigen/Geometry>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#include <stb_loader.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif


PACK_STRUCT(struct VulkanGSFusedProjectionBackwardOptimizerUniforms {
    uint32_t step;
    uint32_t active_sh;
    uint32_t num_splats;
    uint32_t image_size;
    float fx;
    float fy;
    float cx;
    float cy;
    float dist_coeffs[4];
    float lr_means;
    float lr_quats;
    float lr_scales;
    float lr_opacities;
    float reg_scale;
    float reg_opacity;
    float lr_sh_dc;
    float lr_sh_rest;
    float world_view_transform[16];
});

PACK_STRUCT(struct VulkanGSDefaultStrategyUniforms {
    uint32_t num_splats; /* or stride */
    uint32_t old_num_splats;
    uint32_t seed;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t step;
    float prune_opa;
    float grow_grad2d;
    float grow_scale3d;
    float grow_scale2d;
    float prune_scale3d;
    float prune_scale2d;
    int32_t refine_scale2d_stop_iter;
    int32_t refine_start_iter;
    int32_t refine_stop_iter;
    int32_t reset_every;
    int32_t refine_every;
    int32_t pause_refine_after_reset;
});


VulkanGSTrainer::VulkanGSTrainer()
    : VulkanGSRenderer() {
}

VulkanGSTrainer::~VulkanGSTrainer() {
    if (commandBatchInProgress)
        endCommandBatch(false);
    cleanup();
}

void VulkanGSTrainer::cleanup() {
    HOST_GUARD;

    for (auto im : dataset_train) {
        destroyBuffer(im.buffer.deviceBuffer);
        im.buffer.clear();
        im.buffer.shrink_to_fit();
    }
    dataset_train.clear();

    for (auto im : dataset_val) {
        destroyBuffer(im.buffer.deviceBuffer);
        im.buffer.clear();
        im.buffer.shrink_to_fit();
    }
    dataset_val.clear();

    VulkanGSRenderer::cleanup();
}


void VulkanGSTrainer::initialize(const std::map<std::string, std::string> &spirv_paths, int device_id) {

    VulkanGSRenderer::initialize(spirv_paths, device_id);
    
    createComputePipeline(pipeline_ssim_forward, spirv_paths.at("ssim_forward"));
    createComputePipeline(pipeline_ssim_backward, spirv_paths.at("ssim_backward"));
    createComputePipeline(pipeline_fused_projection_backward_optimizer, spirv_paths.at("fused_projection_backward_optimizer"));
    createComputePipeline(pipeline_default.update_state, spirv_paths.at("default_update_state"));
    createComputePipeline(pipeline_default.compute_grow_mask, spirv_paths.at("default_compute_grow_mask"));
    createComputePipeline(pipeline_default.duplicate, spirv_paths.at("default_duplicate"));
    createComputePipeline(pipeline_default.split, spirv_paths.at("default_split"));
    createComputePipeline(pipeline_default.compute_prune_mask, spirv_paths.at("default_compute_prune_mask"));
    createComputePipeline(pipeline_default.prune, spirv_paths.at("default_prune"));
    createComputePipeline(pipeline_default.prune_mean, spirv_paths.at("default_prune_mean"));
    createComputePipeline(pipeline_default.prune_sh, spirv_paths.at("default_prune_sh"));
    createComputePipeline(pipeline_default.reset_opa, spirv_paths.at("default_reset_opa"));
    createComputePipeline(pipeline_mcmc.inject_noise, spirv_paths.at("mcmc_inject_noise"));
    createComputePipeline(pipeline_mcmc.compute_probs, spirv_paths.at("mcmc_compute_probs"));
    createComputePipeline(pipeline_mcmc.compute_relocation_index_map, spirv_paths.at("mcmc_compute_relocation_index_map"));
    createComputePipeline(pipeline_mcmc.compute_relocation, spirv_paths.at("mcmc_compute_relocation"));
    createComputePipeline(pipeline_mcmc.update_relocation, spirv_paths.at("mcmc_update_relocation"));
    createComputePipeline(pipeline_mcmc.compute_add_index_map, spirv_paths.at("mcmc_compute_add_index_map"));
    createComputePipeline(pipeline_mcmc.compute_add, spirv_paths.at("mcmc_compute_add"));
    createComputePipeline(pipeline_mcmc.update_add, spirv_paths.at("mcmc_update_add"));
    createComputePipeline(pipeline_morton_sort.compute_stats, spirv_paths.at("morton_sort_compute_stats"));
    createComputePipeline(pipeline_morton_sort.generate_keys, spirv_paths.at("morton_sort_generate_keys"));
    createComputePipeline(pipeline_morton_sort.apply_indices, spirv_paths.at("morton_sort_apply_indices"));
    createComputePipeline(pipeline_morton_sort.apply_indices_sh, spirv_paths.at("morton_sort_apply_indices_sh"));
    createComputePipeline(pipeline_morton_sort.update_buffer, spirv_paths.at("morton_sort_update_buffer"));
    createComputePipeline(pipeline_morton_sort.update_buffer_sh, spirv_paths.at("morton_sort_update_buffer_sh"));

    rng.seed(42);
}


void VulkanGSTrainer::loadDataset(
    const VkSplatDataset &dataset,
    const TrainerConfig &config,
    VulkanGSPipelineBuffers& buffers
) {
    dataset_train = dataset.train;
    dataset_val = dataset.val;
    if (dataset_train.empty())
        throw std::runtime_error("No training image in the dataset");

    /* Normalize world space using all cameras and sparse points */

    std::vector<Eigen::Matrix4f> c2w_poses;
    c2w_poses.reserve(dataset_train.size() + dataset_val.size());
    for (auto& im : dataset_train) c2w_poses.push_back(im.camera.camtoworld);
    for (auto& im : dataset_val) c2w_poses.push_back(im.camera.camtoworld);

    std::vector<Eigen::Vector3f> points = dataset.points;
    dataparser_transform = normalizeWorldSpace(c2w_poses, points);

    size_t poseIdx = 0;
    for (auto& im : dataset_train) im.camera.camtoworld = c2w_poses[poseIdx++];
    for (auto& im : dataset_val) im.camera.camtoworld = c2w_poses[poseIdx++];

    /* Scene scale from normalized camera positions */

    Eigen::Vector3f scene_center = Eigen::Vector3f::Zero();
    for (const auto& pose : c2w_poses) scene_center += pose.block<3, 1>(0, 3);
    scene_center /= (float)c2w_poses.size();

    scene_scale = 0.0f;
    for (const auto& pose : c2w_poses) {
        float dist = (pose.block<3, 1>(0, 3) - scene_center).norm();
        scene_scale = std::max(scene_scale, dist);
    }
    scene_scale *= 1.1f;  // apple-to-apple with Inria/GSplat
    scene_scale *= config.global_scale;
    printf("Scene scale: %f\n", scene_scale);
    fflush(stdout);

    /* Load images, using multiple threads */

    auto loadImageTask = [](VkSplatDataset::Image& image) {
        auto& camera = image.camera;
        int w = 0, h = 0, c = 0;
        uint8_t* pixels = vksplat::loadImage(image.image_path.c_str(), &w, &h, &c, 4);
        if (!pixels)
            throw std::runtime_error("Failed to load image: " + image.image_path);
        if (h != camera.h || w != camera.w) {
            float sx = (float)w / (float)camera.w;
            float sy = (float)h / (float)camera.h;
            camera.fx *= sx; camera.fy *= sy;
            camera.cx *= sx; camera.cy *= sy;
            camera.w = w; camera.h = h;
        }

        if (!image.mask_path.empty()) {
            int mw = 0, mh = 0, mc = 0;
            uint8_t* mask_pixels = vksplat::loadImage(image.mask_path.c_str(), &mw, &mh, &mc, 1);
            if (!mask_pixels) {
                vksplat::freeImage(pixels);
                throw std::runtime_error("Failed to load mask: " + image.mask_path);
            }
            if (mw != w || mh != h) {
                vksplat::freeImage(mask_pixels); vksplat::freeImage(pixels);
                throw std::runtime_error("Image and mask dimension mismatch: "
                    + image.image_path + " " + image.mask_path);
            }
            for (int i = 0; i < w*h; i++)
                pixels[4*i+3] = ((int)pixels[4*i+3] * (int)mask_pixels[i]) >> 8;
            vksplat::freeImage(mask_pixels);
        }

        image.buffer.resize((size_t)h * (size_t)w * 4);
        image.buffer.assign(&pixels[0], &pixels[h*w*4]);
        vksplat::freeImage(pixels);
    };

    std::vector<VkSplatDataset::Image*> all_images;
    all_images.reserve(dataset_train.size() + dataset_val.size());
    for (auto& im : dataset_train) all_images.push_back(&im);
    for (auto& im : dataset_val) all_images.push_back(&im);

    const size_t numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t batchSize = numThreads * 2;
    for (size_t start = 0; start < all_images.size(); start += batchSize) {
        size_t end = std::min(start + batchSize, all_images.size());
        std::vector<std::future<void>> futures;
        futures.reserve(end - start);
        for (size_t i = start; i < end; i++)
            futures.push_back(std::async(std::launch::async,
                [&loadImageTask, image = all_images[i]] { loadImageTask(*image); }));
        for (auto& f : futures) f.get();
        printf("Loading image %d/%d\r", (int)end, (int)all_images.size());
        fflush(stdout);
    }
    printf("\n");

    int num_train_mask = 0, num_val_mask = 0;
    for (auto& im : dataset_train) num_train_mask += (int)(!im.mask_path.empty());
    for (auto& im : dataset_val) num_val_mask += (int)(!im.mask_path.empty());
    if (num_train_mask != 0 || num_val_mask != 0)
        printf("%d train (%d with mask), %d val (%d with mask), ",
            (int)dataset_train.size(), num_train_mask,
            (int)dataset_val.size(), num_val_mask);
    else
        printf("%d train, %d val, ", (int)dataset_train.size(), (int)dataset_val.size());
    fflush(stdout);

    /* Copy images to device if requested */

    if (config.image_cache_device == TrainerConfig::CacheImage::GPU) {
        for (auto& im : dataset_train) copyToDevice(im.buffer);
        for (auto& im : dataset_val) copyToDevice(im.buffer);
    }

    /* Initialize Gaussians from the sparse point cloud */

    size_t numPoints = points.size();
    if (numPoints == 0)
        throw std::runtime_error("No point in initial point cloud");
    if (dataset.point_colors.size() != numPoints)
        throw std::runtime_error("Point color count does not match point count");

    buffers.num_splats = numPoints;
    const int kNumSH = 16;
    const float kSHC0 = 0.28209479177387814f;
    buffers.xyz_ws.resize(3 * numPoints);
    buffers.sh_coeffs.resize(kNumSH * 3 * numPoints);
    buffers.rotations.resize(4 * numPoints);
    buffers.scales_opacs.resize(4 * numPoints);

    std::normal_distribution<float> unit_normal(0.0, 1.0);

    // find splat scales from nearest neighbor (per GSplat/Nerfstudio)
    NearestNeighbors3D knn;
    knn.fit(points);
    auto [distances, indices] = knn.kneighbors(points, 4);
    std::vector<float> dist_avg;
    for (auto& dists : distances) {
        float sum_dist2 = 0.0f;
        for (float dist : dists) sum_dist2 += dist * dist;
        dist_avg.push_back(sqrt(sum_dist2 / float(dists.size() - 1)));
    }

    for (size_t pointIdx = 0; pointIdx < numPoints; pointIdx++) {
        // position
        buffers.xyz_ws[3*pointIdx+0] = points[pointIdx].x();
        buffers.xyz_ws[3*pointIdx+1] = points[pointIdx].y();
        buffers.xyz_ws[3*pointIdx+2] = points[pointIdx].z();

        // quaternion (per Nerfstudio, uses a different parameterization)
        // Slang reads it as (w, x, y, z); keep the original component order.
        float quat[4] = { unit_normal(rng), unit_normal(rng),
                          unit_normal(rng), unit_normal(rng) };
        float quatLen = sqrtf(quat[0]*quat[0] + quat[1]*quat[1]
                            + quat[2]*quat[2] + quat[3]*quat[3]);
        for (int i = 0; i < 4; i++) buffers.rotations[4*pointIdx+i] = quat[i] / quatLen;

        // scales and opacity
        float scale = config.init_scale * dist_avg[pointIdx];
        float opac = config.init_opacity;
        buffers.scales_opacs[4*pointIdx+0] = scale;
        buffers.scales_opacs[4*pointIdx+1] = scale;
        buffers.scales_opacs[4*pointIdx+2] = scale;
        buffers.scales_opacs[4*pointIdx+3] = opac;

        // SH: DC from point color, rest = 0
        float* sh_base = &buffers.sh_coeffs[kNumSH*3*pointIdx];
        Eigen::Vector3f albedo = dataset.point_colors[pointIdx];
        sh_base[0] = (albedo.x() - 0.5f) / kSHC0;
        sh_base[1] = (albedo.y() - 0.5f) / kSHC0;
        sh_base[2] = (albedo.z() - 0.5f) / kSHC0;
        for (int i = 1; i < kNumSH; i++) {
            sh_base[3*i+0] = 0.0f; sh_base[3*i+1] = 0.0f; sh_base[3*i+2] = 0.0f;
        }
    }

    buffers.reorderSH(buffers.sh_coeffs);

    // MCMC relocates and adds Gaussians; reserve the device buffers up-front so no
    // reallocation is needed during training (matching simple_trainer.py).
    if (config.strategy == TrainerConfig::Strategy::MCMC) {
        resizeDeviceBuffer(buffers.xyz_ws, 3 * config.cap_max);
        resizeDeviceBuffer(buffers.sh_coeffs, 12 * 4 * config.cap_max);
        resizeDeviceBuffer(buffers.rotations, 4 * config.cap_max);
        resizeDeviceBuffer(buffers.scales_opacs, 4 * config.cap_max);
    }

    copyToDevice(buffers.xyz_ws);
    copyToDevice(buffers.sh_coeffs);
    copyToDevice(buffers.rotations);
    copyToDevice(buffers.scales_opacs);

    printf("%d initial points\n", (int)numPoints);
    fflush(stdout);
}


void VulkanGSTrainer::camera_to_uniforms(const VkSplatDataset::Camera &cam,
                                         VulkanGSRendererUniforms &uniforms) const {
    uniforms.image_height = cam.h;
    uniforms.image_width = cam.w;

    uniforms.grid_height = _CEIL_DIV(cam.h, TILE_HEIGHT);
    uniforms.grid_width = _CEIL_DIV(cam.w, TILE_WIDTH);

    uniforms.camera_model =
        (cam.model == cam.SIMPLE_PINHOLE
            || cam.model == cam.PINHOLE
        ) ? 0 :
        (cam.model == cam.SIMPLE_RADIAL
            || cam.model == cam.RADIAL
            || cam.model == cam.OPENCV
            || cam.model == cam.FULL_OPENCV
        ) ? 1:
        (cam.model == cam.SIMPLE_RADIAL_FISHEYE
            || cam.model == cam.RADIAL_FISHEYE
            || cam.model == cam.OPENCV_FISHEYE
            || cam.model == cam.THIN_PRISM_FISHEYE
        ) ? 2: 0xff;

    uniforms.fx = cam.fx;
    uniforms.fy = cam.fy;
    uniforms.cx = cam.cx;
    uniforms.cy = cam.cy;

    switch (cam.model) {
    case cam.SIMPLE_PINHOLE:
    case cam.PINHOLE:
        uniforms.dist_coeffs[0] = 0.0f; uniforms.dist_coeffs[1] = 0.0f;
        uniforms.dist_coeffs[2] = 0.0f; uniforms.dist_coeffs[3] = 0.0f;
        break;
    case cam.SIMPLE_RADIAL:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = 0.0f;
        uniforms.dist_coeffs[2] = 0.0f; uniforms.dist_coeffs[3] = 0.0f;
        break;
    case cam.RADIAL:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = cam.k2;
        uniforms.dist_coeffs[2] = 0.0f; uniforms.dist_coeffs[3] = 0.0f;
        break;
    case cam.OPENCV:
    case cam.FULL_OPENCV:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = cam.k2;
        uniforms.dist_coeffs[2] = cam.p1; uniforms.dist_coeffs[3] = cam.p2;
        break;
    case cam.SIMPLE_RADIAL_FISHEYE:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = 0.0f;
        uniforms.dist_coeffs[2] = 0.0f; uniforms.dist_coeffs[3] = 0.0f;
        break;
    case cam.RADIAL_FISHEYE:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = cam.k2;
        uniforms.dist_coeffs[2] = 0.0f; uniforms.dist_coeffs[3] = 0.0f;
        break;
    case cam.OPENCV_FISHEYE:
    case cam.THIN_PRISM_FISHEYE:
        uniforms.dist_coeffs[0] = cam.k1; uniforms.dist_coeffs[1] = cam.k2;
        uniforms.dist_coeffs[2] = cam.k3; uniforms.dist_coeffs[3] = cam.k4;
        break;
    default:
        _THROW_ERROR("Unsupported camera model");
        break;
    }

    // Eigen matrices are column-major by default, matching the layout of the original
    // glm storage used by the Slang uniform block.
    Eigen::Matrix4f w2c = cam.camtoworld.inverse();
    std::memcpy(uniforms.world_view_transform, w2c.data(), 16 * sizeof(float));
}


void VulkanGSTrainer::get_train_camera(size_t idx, VulkanGSRendererUniforms &uniforms) const {
    camera_to_uniforms(dataset_train[idx].camera, uniforms);
}

void VulkanGSTrainer::get_val_camera(size_t idx, VulkanGSRendererUniforms &uniforms) const {
    camera_to_uniforms(dataset_val[idx].camera, uniforms);
}


void VulkanGSTrainer::executeComputeSSIMGradient(
    const TrainerConfig &config,
    const VulkanGSRendererUniforms& uniforms,
    VulkanGSPipelineBuffers& buffers,
    size_t train_idx
) {
    size_t w = uniforms.image_width, h = uniforms.image_height;
    size_t num_pixels = w * h;
    Uniform32_t shader_uniforms[4];
    shader_uniforms[0].u = (uint32_t)w;
    shader_uniforms[1].u = (uint32_t)h;
    const uint32_t kTwoHalo = 10;
    shader_uniforms[2].f = (1.0f - config.ssim_lambda) / (3.0f * w * h);  // l1 grad weight
    shader_uniforms[3].f = -config.ssim_lambda / (3.0f * (w-kTwoHalo) * (h-kTwoHalo));  // ssim grad weight
    
    auto& train_image = dataset_train[train_idx].buffer;
    bool buffer_swapped = false;
    if (train_image.deviceBuffer.buffer == VK_NULL_HANDLE) {
        PerfTimer::Timer<PerfTimer::CopyTrainImageToDevice> timer(this);
        std::swap(train_image.deviceBuffer, buffers.ref_image.deviceBuffer);
        copyToDevice(train_image);
        buffer_swapped = true;
    }
    bufferMemoryBarrier({
        { buffers.pixel_state.deviceBuffer, COMPUTE_SHADER_WRITE },
        { train_image.deviceBuffer, TRANSFER_WRITE },
    }, COMPUTE_SHADER_READ);

    PerfTimer::Timer<PerfTimer::ComputeSSIMGradient> timer(this);

    DEVICE_GUARD;

    // auto& ssim_map = buffers.ssim_map;
    auto& ssim_map = buffers._temp_gauss_attr;  // reuse buffer to save VRAM

    executeCompute(
        {{w, 16}, {h, 16}},
        shader_uniforms, 4*sizeof(Uniform32_t),
        pipeline_ssim_forward,
        {
            buffers.pixel_state.deviceBuffer,
            train_image.deviceBuffer,
            resizeDeviceBuffer(ssim_map, 12*num_pixels),
        }
    );

    bufferMemoryBarrier({
        { ssim_map.deviceBuffer, COMPUTE_SHADER_WRITE },
    }, COMPUTE_SHADER_READ);
    executeCompute(
        {{w, 16}, {h, 16}},
        shader_uniforms, 3*sizeof(Uniform32_t),
        pipeline_ssim_backward,
        {
            buffers.pixel_state.deviceBuffer,
            train_image.deviceBuffer,
            ssim_map.deviceBuffer,
            resizeDeviceBuffer(buffers.v_pixel_state, 4*num_pixels),
        }
    );

    if (buffer_swapped)
        std::swap(train_image.deviceBuffer, buffers.ref_image.deviceBuffer);
}


void VulkanGSTrainer::executeFusedProjectionBackwardOptimizerStep(
    const TrainerConfig &config,
    const VulkanGSRendererUniforms& uniforms,
    VulkanGSPipelineBuffers& buffers,
    int step
) {
    PerfTimer::Timer<PerfTimer::FusedProjectionBackwardOptimizerStep> timer(this);
    DEVICE_GUARD;

    size_t num_splats = buffers.num_splats;

    bufferMemoryBarrier({
        { buffers.tiles_touched.deviceBuffer, COMPUTE_SHADER_WRITE },
        { buffers.v_inv_cov_vs_opacity.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.v_rgb.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
    }, COMPUTE_SHADER_READ);

    VulkanGSFusedProjectionBackwardOptimizerUniforms shaderUniforms;
    shaderUniforms.step = step;
    shaderUniforms.active_sh = (uniforms.camera_model << 8) | uniforms.active_sh;
    shaderUniforms.num_splats = (uint32_t)num_splats;
    shaderUniforms.image_size = (uniforms.image_height << 16) | uniforms.image_width;
    shaderUniforms.fx = uniforms.fx;
    shaderUniforms.fy = uniforms.fy;
    shaderUniforms.cx = uniforms.cx;
    shaderUniforms.cy = uniforms.cy;
    for (int i = 0; i < 4; i++)
        shaderUniforms.dist_coeffs[i] = uniforms.dist_coeffs[i];
    shaderUniforms.lr_means = config.get_means_lr(step, scene_scale);
    shaderUniforms.lr_quats = config.quats_lr;
    shaderUniforms.lr_scales = config.scales_lr;
    shaderUniforms.lr_opacities = config.opacities_lr;
    shaderUniforms.reg_scale = config.scale_reg / (3*num_splats);
    shaderUniforms.reg_opacity = config.opacity_reg / num_splats;
    shaderUniforms.lr_sh_dc = config.features_dc_lr;
    shaderUniforms.lr_sh_rest = config.features_rest_lr;
    for (size_t i = 0; i < 16; i++)
        shaderUniforms.world_view_transform[i] = uniforms.world_view_transform[i];

    // execute compute
    size_t alloc_size = num_splats;
    if (config.strategy == TrainerConfig::Strategy::MCMC)
        alloc_size = std::max(alloc_size, (size_t)config.cap_max);
    executeCompute(
        {{num_splats, SUBGROUP_SIZE}},
        &shaderUniforms, sizeof(shaderUniforms),
        pipeline_fused_projection_backward_optimizer,
        {
            buffers.xyz_ws.deviceBuffer,
            buffers.sh_coeffs.deviceBuffer,
            buffers.rotations.deviceBuffer,
            buffers.scales_opacs.deviceBuffer,
            buffers.tiles_touched.deviceBuffer,
            buffers.v_xy_vs.deviceBuffer,
            buffers.v_inv_cov_vs_opacity.deviceBuffer,
            buffers.v_rgb.deviceBuffer,
            resizeAndCopyDeviceBuffer(buffers.g_xyz_ws, 2*3*alloc_size, true),
            resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_1, 16*3*_CEIL_ROUND(alloc_size,SH_REORDER_SIZE), true),
            resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_2, 16*3*_CEIL_ROUND(alloc_size,SH_REORDER_SIZE), true),
            resizeAndCopyDeviceBuffer(buffers.g_rotations, 2*4*alloc_size, true),
            resizeAndCopyDeviceBuffer(buffers.g_scales_opacs, 2*4*alloc_size, true),
        }
    );
}


void VulkanGSTrainer::barrierAllGaussParams(VulkanGSPipelineBuffers& buffers) {
    bufferMemoryBarrier({
        { buffers.xyz_ws.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.sh_coeffs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.rotations.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.scales_opacs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.g_xyz_ws.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.g_sh_coeffs_1.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.g_sh_coeffs_2.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.g_rotations.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.g_scales_opacs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.default_grad.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.default_radii.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
    }, COMPUTE_SHADER_READ_WRITE);
}

void VulkanGSTrainer::executeDefaultPostBackward(
    const TrainerConfig &config,
    const VulkanGSRendererUniforms& renderer_uniforms,
    VulkanGSPipelineBuffers& buffers,
    int step
) {
    PerfTimer::Timer<PerfTimer::DefaultPostBackward> timer(this);
    DEVICE_GUARD;

    size_t num_splats = buffers.num_splats;
    const uint32_t width = renderer_uniforms.image_width;
    const uint32_t height = renderer_uniforms.image_height;

    VulkanGSDefaultStrategyUniforms uniforms;
    uniforms.num_splats = (uint32_t)num_splats;
    uniforms.old_num_splats = (uint32_t)num_splats;
    uniforms.seed = std::uniform_int_distribution<uint32_t>()(rng);
    uniforms.image_width = width;
    uniforms.image_height = height;
    uniforms.step = step;
    uniforms.prune_opa = config.prune_opa;
    uniforms.grow_grad2d = config.grow_grad2d;
    uniforms.grow_scale3d = config.grow_scale3d * scene_scale;
    uniforms.grow_scale2d = config.grow_scale2d;
    uniforms.prune_scale3d = config.prune_scale3d * scene_scale;
    uniforms.prune_scale2d = config.prune_scale2d;
    uniforms.refine_scale2d_stop_iter = config.refine_scale2d_stop_iter;
    uniforms.refine_start_iter = config.refine_start_iter;
    uniforms.refine_stop_iter = config.refine_stop_iter;
    uniforms.reset_every = config.reset_every;
    uniforms.refine_every = config.refine_every;
    uniforms.pause_refine_after_reset = config.pause_refine_after_reset >= 0 ?
        config.pause_refine_after_reset :
        (int32_t)dataset_train.size() + uniforms.refine_every;

    if (step >= uniforms.refine_stop_iter)
        return;

    bufferMemoryBarrier({
        { buffers.v_xy_vs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.radii.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.scales_opacs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.default_grad.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.default_radii.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
    }, COMPUTE_SHADER_READ_WRITE);

    executeCompute(
        {{num_splats, 256}},
        &uniforms, sizeof(uniforms),
        pipeline_default.update_state,
        {
            buffers.v_xy_vs.deviceBuffer,
            buffers.radii.deviceBuffer,
            resizeDeviceBuffer(buffers.default_grad, 2*num_splats),
            resizeDeviceBuffer(buffers.default_radii, num_splats),
        }
    );
    bufferMemoryBarrier({
        { buffers.default_grad.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        { buffers.default_radii.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
    }, COMPUTE_SHADER_READ);

    if (step > uniforms.refine_start_iter
        && step % uniforms.refine_every == 0
        && (step % uniforms.reset_every >= uniforms.pause_refine_after_reset)
    ) {
        // grow GS

        executeCompute(
            {{num_splats, 256}},
            &uniforms, sizeof(uniforms),
            pipeline_default.compute_grow_mask,
            {
                buffers.default_grad.deviceBuffer,
                buffers.default_radii.deviceBuffer,
                buffers.scales_opacs.deviceBuffer,
                resizeDeviceBuffer(buffers.default_dupli_mask, num_splats),
                resizeDeviceBuffer(buffers.default_split_mask, num_splats),
            }
        );
        bufferMemoryBarrier({
            { buffers.default_dupli_mask.deviceBuffer, COMPUTE_SHADER_WRITE },
            { buffers.default_split_mask.deviceBuffer, COMPUTE_SHADER_WRITE },
        }, COMPUTE_SHADER_READ);

        int num_dupli = executeSum(buffers, buffers.default_dupli_mask);
        int num_split = executeSum(buffers, buffers.default_split_mask);

        if (num_dupli + num_split > 0) {
            size_t new_num_splats = num_splats + num_dupli + num_split;
            barrierAllGaussParams(buffers);
            resizeAndCopyDeviceBuffer(buffers.xyz_ws, 3*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.sh_coeffs, 16*3*_CEIL_ROUND(new_num_splats,SH_REORDER_SIZE), false);
            resizeAndCopyDeviceBuffer(buffers.rotations, 4*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.scales_opacs, 4*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.g_xyz_ws, 3*2*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_1, 16*3*_CEIL_ROUND(new_num_splats,SH_REORDER_SIZE), false);
            resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_2, 16*3*_CEIL_ROUND(new_num_splats,SH_REORDER_SIZE), false);
            resizeAndCopyDeviceBuffer(buffers.g_rotations, 4*2*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.g_scales_opacs, 4*2*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.default_grad, 2*new_num_splats, false);
            resizeAndCopyDeviceBuffer(buffers.default_radii, new_num_splats, false);
            buffers.num_splats = new_num_splats;
            barrierAllGaussParams(buffers);
        }

        if (num_dupli > 0) {
            executeWhere(buffers, buffers.default_dupli_mask, buffers._temp_indices);
            bufferMemoryBarrier({
                { buffers._temp_indices.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
            }, COMPUTE_SHADER_READ);

            uniforms.num_splats = (uint32_t)(num_splats + num_dupli);
            uniforms.old_num_splats = (uint32_t)(num_splats);
            executeCompute(
                {{num_dupli, 256}},
                &uniforms, sizeof(uniforms),
                pipeline_default.duplicate,
                {
                    buffers._temp_indices.deviceBuffer,
                    buffers.xyz_ws.deviceBuffer,
                    buffers.sh_coeffs.deviceBuffer,
                    buffers.rotations.deviceBuffer,
                    buffers.scales_opacs.deviceBuffer,
                    buffers.g_xyz_ws.deviceBuffer,
                    buffers.g_sh_coeffs_1.deviceBuffer,
                    buffers.g_sh_coeffs_2.deviceBuffer,
                    buffers.g_rotations.deviceBuffer,
                    buffers.g_scales_opacs.deviceBuffer,
                    buffers.default_grad.deviceBuffer,
                    buffers.default_radii.deviceBuffer,
                }
            );
            barrierAllGaussParams(buffers);
        }

        if (num_split > 0) {
            executeWhere(buffers, buffers.default_split_mask, buffers._temp_indices);
            bufferMemoryBarrier({
                { buffers._temp_indices.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
            }, COMPUTE_SHADER_READ);

            uniforms.num_splats = (uint32_t)(num_splats + num_dupli + num_split);
            uniforms.old_num_splats = (uint32_t)(num_splats + num_dupli);
            executeCompute(
                {{num_split, 256}},
                &uniforms, sizeof(uniforms),
                pipeline_default.split,
                {
                    buffers._temp_indices.deviceBuffer,
                    buffers.xyz_ws.deviceBuffer,
                    buffers.sh_coeffs.deviceBuffer,
                    buffers.rotations.deviceBuffer,
                    buffers.scales_opacs.deviceBuffer,
                    buffers.g_xyz_ws.deviceBuffer,
                    buffers.g_sh_coeffs_1.deviceBuffer,
                    buffers.g_sh_coeffs_2.deviceBuffer,
                    buffers.g_rotations.deviceBuffer,
                    buffers.g_scales_opacs.deviceBuffer,
                    buffers.default_grad.deviceBuffer,
                    buffers.default_radii.deviceBuffer,
                }
            );
            barrierAllGaussParams(buffers);
        }

        // prune GS

        num_splats = buffers.num_splats;
        uniforms.num_splats = (uint32_t)num_splats;
        uniforms.old_num_splats = (uint32_t)num_splats;
        executeCompute(
            {{num_splats, 256}},
            &uniforms, sizeof(uniforms),
            pipeline_default.compute_prune_mask,
            {
                buffers.scales_opacs.deviceBuffer,
                buffers.default_radii.deviceBuffer,
                resizeDeviceBuffer(buffers.default_keep_mask, num_splats),
            }
        );
        bufferMemoryBarrier({
            { buffers.default_keep_mask.deviceBuffer, COMPUTE_SHADER_WRITE },
        }, COMPUTE_SHADER_READ);

        int num_keep = executeSum(buffers, buffers.default_keep_mask);
        int num_prune = (int)num_splats - num_keep;

        if (num_prune > 0) {
            executeCumsum(buffers, buffers.default_keep_mask, buffers._temp_indices);
            
            enum PruneType {
                Default, Sh, Mean
            };
            auto applyPrune = [&](Buffer<float> &buffer, PruneType type=Default) {
                uint32_t stride = (uint32_t)(buffer.deviceSize() / num_splats);
                uniforms.old_num_splats = (uint32_t)(num_splats);
                uniforms.num_splats = stride / (type == Sh ? 12*4 : type == Mean ? 3 : 1);  // stride
                size_t num_keep_ceil = num_keep,
                    num_splats_ceil = num_splats;
                if (type == Sh || type == Mean) {
                    size_t REORDER_SIZE = type == Sh ? SH_REORDER_SIZE : 1;
                    num_keep_ceil = _CEIL_ROUND(num_keep, REORDER_SIZE);
                    num_splats_ceil = _CEIL_ROUND(num_splats, REORDER_SIZE);
                }
                resizeDeviceBuffer(buffers._temp_gauss_attr, num_keep_ceil*stride);
                executeCompute(
                    {{num_splats_ceil*(type == Default ? stride : 1), 256}},
                    &uniforms, sizeof(uniforms),
                    type == Mean ? pipeline_default.prune_mean :
                        type == Sh ? pipeline_default.prune_sh :
                        pipeline_default.prune,
                    {
                        buffers.default_keep_mask.deviceBuffer,
                        buffers._temp_indices.deviceBuffer,
                        buffer.deviceBuffer,
                        buffers._temp_gauss_attr.deviceBuffer,
                    }
                );
                bufferMemoryBarrier({
                    { buffers._temp_gauss_attr.deviceBuffer, COMPUTE_SHADER_WRITE }
                }, TRANSFER_COMPUTE_SHADER_READ_WRITE);
                copyFromDeviceToDevice(buffers._temp_gauss_attr.deviceBuffer, buffer.deviceBuffer);
                bufferMemoryBarrier({
                    { buffer.deviceBuffer, COMPUTE_SHADER_WRITE }
                }, TRANSFER_READ_WRITE);
            };
            applyPrune(buffers.sh_coeffs, Sh);  // do the largest first since buffers._temp_indices reallocs are lazy
            applyPrune(buffers.g_sh_coeffs_1, Sh);
            applyPrune(buffers.g_sh_coeffs_2, Sh);
            applyPrune(buffers.xyz_ws, Mean);
            applyPrune(buffers.rotations);
            applyPrune(buffers.scales_opacs);
            applyPrune(buffers.g_xyz_ws, Mean);
            applyPrune(buffers.g_rotations);
            applyPrune(buffers.g_scales_opacs);
            // running states - default_grad will be cleared anyway, default_radii might be needed
            if (!(config.refine_scale2d_stop_iter > 0))
                applyPrune(buffers.default_radii);
            #undef applyPrune
            barrierAllGaussParams(buffers);

            buffers.num_splats = num_keep;
        }

        // reset running states
        clearDeviceBuffer(buffers.default_grad, 2*buffers.num_splats);
        if (config.refine_scale2d_stop_iter > 0)
            clearDeviceBuffer(buffers.default_radii, buffers.num_splats);
        bufferMemoryBarrier({
            { buffers.default_grad.deviceBuffer, TRANSFER_WRITE },
            { buffers.default_radii.deviceBuffer, TRANSFER_WRITE },
        }, COMPUTE_SHADER_READ_WRITE);

        printf("\n%d dupli, %d split, %d prune -> %d splats\n",
            num_dupli, num_split, num_prune, (int)buffers.num_splats);
        fflush(stdout);
    }

    // reset opacity
    if (step % uniforms.reset_every == 0 && step > 0 &&
        (config.stop_reset_at == -1 || step <= config.stop_reset_at)
    ) {
        num_splats = buffers.num_splats;
        uniforms.num_splats = (uint32_t)num_splats;
        uniforms.old_num_splats = (uint32_t)num_splats;
        executeCompute(
            {{num_splats, 256}},
            &uniforms, sizeof(uniforms),
            pipeline_default.reset_opa,
            {
                buffers.scales_opacs.deviceBuffer,
                buffers.g_scales_opacs.deviceBuffer,
            }
        );
        bufferMemoryBarrier({
            { buffers.scales_opacs.deviceBuffer, COMPUTE_SHADER_WRITE },
            { buffers.g_scales_opacs.deviceBuffer, COMPUTE_SHADER_WRITE },
        }, COMPUTE_SHADER_READ);
        printf("\nreset opacity\n");
        fflush(stdout);
    }

  #if 1
    if (step % config.refine_every == 0) {
        executeMortonSorting(renderer_uniforms, buffers);
    }
  #endif
}


void VulkanGSTrainer::executeMCMCPostBackward(
    const TrainerConfig &config,
    const VulkanGSRendererUniforms& renderer_uniforms,
    VulkanGSPipelineBuffers& buffers,
    int step
) {
    PerfTimer::Timer<PerfTimer::MCMCPostBackward> timer(this);
    DEVICE_GUARD;

    const size_t kGroupSize = 256;
    const size_t kGroupSizeSparse = 64;
    size_t num_splats = buffers.num_splats;

    Uniform32_t uniforms[5];
    uniforms[0].u = (uint32_t)num_splats;
    uniforms[1].u = 0;  // num_add
    uniforms[2].u = std::uniform_int_distribution<uint32_t>()(rng);
    uniforms[3].f = config.min_opacity;

    // refinement
    if (
        step < config.refine_stop_iter && step > config.refine_start_iter
        && step % config.refine_every == 0
    ) {

        // Relocation */

        // 1) Compute relocation probabilities for all Gaussians
        bufferMemoryBarrier({
            { buffers.scales_opacs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
        }, COMPUTE_SHADER_READ);
        executeCompute(
            {{num_splats, kGroupSize}},
            &uniforms, 4*sizeof(Uniform32_t),
            pipeline_mcmc.compute_probs,
            {
                buffers.scales_opacs.deviceBuffer,
                resizeDeviceBuffer(buffers.mcmc_sample_probs, num_splats),
            }
        );

        // 2) Prefix sum the probability array
        bufferMemoryBarrier({
            { buffers.mcmc_sample_probs.deviceBuffer, COMPUTE_SHADER_WRITE },
        }, COMPUTE_SHADER_READ);
        executeCumsum(
            buffers,
            buffers.mcmc_sample_probs,
            buffers.mcmc_sample_probs_cumsum
        );

        // 3) Compute relocation index map
        clearDeviceBuffer(buffers.mcmc_n_idx_buffer, num_splats);
        bufferMemoryBarrier({
            { buffers.mcmc_sample_probs_cumsum.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
            { buffers.mcmc_n_idx_buffer.deviceBuffer, TRANSFER_WRITE },
        }, COMPUTE_SHADER_READ_WRITE);
        executeCompute(
            {{num_splats, kGroupSize}},
            &uniforms, 4*sizeof(Uniform32_t),
            pipeline_mcmc.compute_relocation_index_map,
            {
                buffers.mcmc_sample_probs.deviceBuffer,
                buffers.mcmc_sample_probs_cumsum.deviceBuffer,
                resizeDeviceBuffer(buffers.mcmc_index_map, num_splats),
                buffers.mcmc_n_idx_buffer.deviceBuffer,
            }
        );

        // (optional) get number of relocated Gaussians for printing
        int32_t num_relocate = executeSum(buffers, buffers.mcmc_n_idx_buffer);

        if (num_relocate > 0) {

            // 4) Compute attributes for Gaussians relocated to
            barrierAllGaussParams(buffers);
            executeCompute(
                {{num_splats, kGroupSizeSparse}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.compute_relocation,
                {
                    buffers.mcmc_n_idx_buffer.deviceBuffer,
                    buffers.scales_opacs.deviceBuffer,
                    buffers.g_xyz_ws.deviceBuffer,
                    buffers.g_sh_coeffs_1.deviceBuffer,
                    buffers.g_sh_coeffs_2.deviceBuffer,
                    buffers.g_rotations.deviceBuffer,
                    buffers.g_scales_opacs.deviceBuffer,
                }
            );

            // 5) Update attributes for Gaussians relocated from
            barrierAllGaussParams(buffers);
            executeCompute(
                {{num_splats, kGroupSizeSparse}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.update_relocation,
                {
                    buffers.mcmc_index_map.deviceBuffer,
                    buffers.xyz_ws.deviceBuffer,
                    buffers.sh_coeffs.deviceBuffer,
                    buffers.rotations.deviceBuffer,
                    buffers.scales_opacs.deviceBuffer,
                    buffers.g_xyz_ws.deviceBuffer,
                    buffers.g_sh_coeffs_1.deviceBuffer,
                    buffers.g_sh_coeffs_2.deviceBuffer,
                    buffers.g_rotations.deviceBuffer,
                    buffers.g_scales_opacs.deviceBuffer,
                }
            );

        }

        // Add more GS */

        size_t new_num_splats = std::min((size_t)(config.grow_factor * num_splats), (size_t)config.cap_max);
        size_t num_add = std::max(new_num_splats, num_splats) - num_splats;
        if (num_add > 0) {
            uniforms[1].u = (uint32_t)num_add;
            
            // 1) Compute relocation probabilities for all Gaussians
            bufferMemoryBarrier({
                { buffers.scales_opacs.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
            }, COMPUTE_SHADER_READ);
            executeCompute(
                {{num_splats, kGroupSize}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.compute_probs,
                {
                    buffers.scales_opacs.deviceBuffer,
                    buffers.mcmc_sample_probs.deviceBuffer,
                }
            );

            // 2) Prefix sum the probability array
            bufferMemoryBarrier({
                { buffers.mcmc_sample_probs.deviceBuffer, COMPUTE_SHADER_WRITE },
            }, COMPUTE_SHADER_READ);
            executeCumsum(
                buffers,
                buffers.mcmc_sample_probs,
                buffers.mcmc_sample_probs_cumsum
            );

            // 3) [dN] Compute add index map
            clearDeviceBuffer(buffers.mcmc_n_idx_buffer, num_splats);
            bufferMemoryBarrier({
                { buffers.mcmc_sample_probs_cumsum.deviceBuffer, COMPUTE_SHADER_READ_WRITE },
                { buffers.mcmc_n_idx_buffer.deviceBuffer, TRANSFER_WRITE },
            }, COMPUTE_SHADER_READ_WRITE);
            executeCompute(
                {{num_add, kGroupSize}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.compute_add_index_map,
                {
                    buffers.mcmc_sample_probs_cumsum.deviceBuffer,
                    resizeDeviceBuffer(buffers.mcmc_index_map, num_add),
                    buffers.mcmc_n_idx_buffer.deviceBuffer,
                }
            );

            // 4) [N] Compute attributes for Gaussians added to
            bufferMemoryBarrier({
                { buffers.mcmc_n_idx_buffer.deviceBuffer, COMPUTE_SHADER_WRITE },
            }, COMPUTE_SHADER_READ);
            executeCompute(
                {{num_splats, kGroupSizeSparse}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.compute_add,
                {
                    buffers.mcmc_n_idx_buffer.deviceBuffer,
                    buffers.scales_opacs.deviceBuffer,
                }
            );

            // 5) [dN] Update attributes for Gaussians added from
            size_t alloc_size = num_splats;
            if (config.strategy == TrainerConfig::Strategy::MCMC)
                alloc_size = std::max(alloc_size, (size_t)config.cap_max);
            barrierAllGaussParams(buffers);
            executeCompute(
                {{num_add, kGroupSize}},
                &uniforms, 4*sizeof(Uniform32_t),
                pipeline_mcmc.update_add,
                {
                    buffers.mcmc_index_map.deviceBuffer,
                    resizeAndCopyDeviceBuffer(buffers.xyz_ws, 3*alloc_size, false),
                    resizeAndCopyDeviceBuffer(buffers.sh_coeffs, 16*3*_CEIL_ROUND(alloc_size,SH_REORDER_SIZE), false),
                    resizeAndCopyDeviceBuffer(buffers.rotations, 4*alloc_size, false),
                    resizeAndCopyDeviceBuffer(buffers.scales_opacs, 4*alloc_size, false),
                    resizeAndCopyDeviceBuffer(buffers.g_xyz_ws, 3*2*alloc_size, false),
                    resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_1, 16*3*_CEIL_ROUND(alloc_size,SH_REORDER_SIZE), false),
                    resizeAndCopyDeviceBuffer(buffers.g_sh_coeffs_2, 16*3*_CEIL_ROUND(alloc_size,SH_REORDER_SIZE), false),
                    resizeAndCopyDeviceBuffer(buffers.g_rotations, 4*2*alloc_size, false),
                    resizeAndCopyDeviceBuffer(buffers.g_scales_opacs, 4*2*alloc_size, false),
                }
            );

            buffers.num_splats = new_num_splats;
        }

        printf("\n%d relocate, %d add -> %d splats\n", num_relocate, (int)num_add, (int)buffers.num_splats);
        fflush(stdout);

    }

  #if 1
    if (step % config.refine_every == 0) {
        executeMortonSorting(renderer_uniforms, buffers);
    }
  #endif

    // inject noise
    uniforms[1].u = uniforms[2].u;  // seed
    uniforms[2].u = renderer_uniforms.image_width;
    uniforms[3].u = renderer_uniforms.image_height;
    uniforms[4].f = config.noise_lr * config.get_means_lr(step, scene_scale);
    bufferMemoryBarrier({
        { buffers.rotations.deviceBuffer, TRANSFER_COMPUTE_SHADER_READ_WRITE },
        { buffers.scales_opacs.deviceBuffer, TRANSFER_COMPUTE_SHADER_READ_WRITE },
        { buffers.xyz_ws.deviceBuffer, TRANSFER_COMPUTE_SHADER_READ_WRITE },
    }, COMPUTE_SHADER_READ_WRITE);
    executeCompute(
        {{num_splats, kGroupSize}},
        &uniforms, 5*sizeof(Uniform32_t),
        pipeline_mcmc.inject_noise,
        {
            buffers.radii.deviceBuffer,
            buffers.rotations.deviceBuffer,
            buffers.xyz_ws.deviceBuffer,
            buffers.scales_opacs.deviceBuffer,
        }
    );

}


void VulkanGSTrainer::executeMortonSorting(
    const VulkanGSRendererUniforms &uniforms,
    VulkanGSPipelineBuffers& buffers
) {
    // PerfTimer::Timer<PerfTimer::MortonSorting> timer(this);
    DEVICE_GUARD;

    uint32_t num_splats = (uint32_t)buffers.num_splats;

    barrierAllGaussParams(buffers);
    executeCompute(
        {{num_splats, SUBGROUP_SIZE*SUBGROUP_SIZE}},
        &num_splats, sizeof(uint32_t),
        pipeline_morton_sort.compute_stats,
        {
            buffers.xyz_ws.deviceBuffer,
            clearDeviceBuffer(buffers._temp_sum, 6),
        }
    );

    bufferMemoryBarrier({
        { buffers._temp_sum.deviceBuffer, COMPUTE_SHADER_WRITE },
    }, COMPUTE_SHADER_READ);
    executeCompute(
        {{num_splats, 256}},
        &num_splats, sizeof(uint32_t),
        pipeline_morton_sort.generate_keys,
        {
            buffers.xyz_ws.deviceBuffer,
            buffers._temp_sum.deviceBuffer,
            resizeDeviceBuffer(buffers.unsorted_keys(), num_splats),
            resizeDeviceBuffer(buffers.unsorted_gauss_idx(), num_splats)
        }
    );

    // TODO: fix double timer count
    executeSort(uniforms, buffers, 32);

    bufferMemoryBarrier({
        { buffers.sorted_gauss_idx().deviceBuffer, COMPUTE_SHADER_WRITE },
    }, COMPUTE_SHADER_READ);

    auto applyIndex = [&](Buffer<float> &buffer, uint32_t stride, bool is_sh=false) {
        if (buffer.deviceSize() == 0)
            return;
        if (SH_REORDER_SIZE == 1)
            is_sh = false;
        uint32_t num_splats_ceil = _CEIL_ROUND(num_splats, is_sh ? SH_REORDER_SIZE : 1);
        for (int offset = 0; offset < (is_sh ? 12 : 1); ++offset) {
            uint32_t uniform[2] = {
                num_splats_ceil,
                is_sh ? offset : stride
            };
            executeCompute(
                {{num_splats_ceil*(is_sh ? 1 : stride), 1024}},
                &uniform, 2*sizeof(uniform),
                is_sh ? pipeline_morton_sort.apply_indices_sh :
                    pipeline_morton_sort.apply_indices,
                {
                    buffers.sorted_gauss_idx().deviceBuffer,
                    buffer.deviceBuffer,
                    resizeDeviceBuffer(buffers._temp_gauss_attr, (is_sh ? 4 : stride)*num_splats_ceil)
                }
            );
            bufferMemoryBarrier({
                { buffer.deviceBuffer, COMPUTE_SHADER_READ },
                { buffers._temp_gauss_attr.deviceBuffer, COMPUTE_SHADER_WRITE },
            }, COMPUTE_SHADER_READ_WRITE);
            executeCompute(
                {{num_splats_ceil*(is_sh ? 1 : stride), 1024}},
                &uniform, 2*sizeof(uniform),
                is_sh ? pipeline_morton_sort.update_buffer_sh :
                    pipeline_morton_sort.update_buffer,
                {
                    buffers._temp_gauss_attr.deviceBuffer,
                    buffer.deviceBuffer,
                }
            );
            bufferMemoryBarrier({
                { buffers._temp_gauss_attr.deviceBuffer, COMPUTE_SHADER_READ },
            }, COMPUTE_SHADER_WRITE);
            // copyFromDeviceToDevice(buffers._temp_gauss_attr, buffer);
        }
    };

    barrierAllGaussParams(buffers);
    applyIndex(buffers.sh_coeffs, 12*4, true);  // do the largest first since buffers._temp_indices reallocs are lazy
    applyIndex(buffers.g_sh_coeffs_1, 12*4, true);
    applyIndex(buffers.g_sh_coeffs_2, 12*4, true);
    applyIndex(buffers.xyz_ws, 3);
    applyIndex(buffers.rotations, 4);
    applyIndex(buffers.scales_opacs, 4);
    applyIndex(buffers.g_xyz_ws, 3*2);
    applyIndex(buffers.g_rotations, 4*2);
    applyIndex(buffers.g_scales_opacs, 4*2);
    applyIndex(buffers.default_radii, 2);
    applyIndex(buffers.default_grad, 2*2);
    barrierAllGaussParams(buffers);

}



void VulkanGSTrainer::writePLY(std::string filename, VulkanGSPipelineBuffers& buffers) {

    // Work on copies so that PLY-specific transforms (log scale, logit opacity, SH undo
    // reorder) never mutate the training buffers, which may still be used to build a
    // GaussianGeometry afterwards.
    size_t num_splats = buffers.num_splats;
    Buffer<float> xyz_ws, sh_coeffs, rotations, scales_opacs;
    xyz_ws.deviceBuffer = buffers.xyz_ws.deviceBuffer;
    sh_coeffs.deviceBuffer = buffers.sh_coeffs.deviceBuffer;
    rotations.deviceBuffer = buffers.rotations.deviceBuffer;
    scales_opacs.deviceBuffer = buffers.scales_opacs.deviceBuffer;
    copyFromDevice(xyz_ws);
    copyFromDevice(sh_coeffs);
    copyFromDevice(rotations);
    copyFromDevice(scales_opacs);
    VulkanGSPipelineBuffers::undoReorderSH(sh_coeffs, num_splats);

    for (size_t i = 0; i < num_splats; i++) {
        float* p = &scales_opacs[4*i];
        p[0] = logf(p[0]);
        p[1] = logf(p[1]);
        p[2] = logf(p[2]);
        p[3] = logf(p[3] / (1.0f - p[3]));
    }

    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) throw std::runtime_error("writePLY: file open failed " + filename);

    struct PLYComponent {
        int n;
        float* buf;
        std::vector<std::string> names;
        std::vector<int> permute;
    };
    std::vector<PLYComponent> components({
        { 3, xyz_ws.data(), { "x", "y", "z" }, { 0, 1, 2 } },
        // { 3, nullptr, { "nx", "ny", "nz" }, { 0, 1, 2 } },
        { 48, sh_coeffs.data(),
            // [f"f_dc_{i}" if i<3 else f"f_rest_{i-3}" for i in range(48)]
            { "f_dc_0", "f_dc_1", "f_dc_2", "f_rest_0", "f_rest_1", "f_rest_2", "f_rest_3", "f_rest_4", "f_rest_5", "f_rest_6", "f_rest_7", "f_rest_8", "f_rest_9", "f_rest_10", "f_rest_11", "f_rest_12", "f_rest_13", "f_rest_14", "f_rest_15", "f_rest_16", "f_rest_17", "f_rest_18", "f_rest_19", "f_rest_20", "f_rest_21", "f_rest_22", "f_rest_23", "f_rest_24", "f_rest_25", "f_rest_26", "f_rest_27", "f_rest_28", "f_rest_29", "f_rest_30", "f_rest_31", "f_rest_32", "f_rest_33", "f_rest_34", "f_rest_35", "f_rest_36", "f_rest_37", "f_rest_38", "f_rest_39", "f_rest_40", "f_rest_41", "f_rest_42", "f_rest_43", "f_rest_44" },
            // [i if i < 3 else (i%3)*15+(i//3)+2 for i in range(48)]
            { 0, 1, 2, 3, 18, 33, 4, 19, 34, 5, 20, 35, 6, 21, 36, 7, 22, 37, 8, 23, 38, 9, 24, 39, 10, 25, 40, 11, 26, 41, 12, 27, 42, 13, 28, 43, 14, 29, 44, 15, 30, 45, 16, 31, 46, 17, 32, 47 }
        },
        { 4, scales_opacs.data(), { "opacity", "scale_0", "scale_1", "scale_2" }, { 1, 2, 3, 0 } },
        { 4, rotations.data(), { "rot_0", "rot_1", "rot_2", "rot_3" }, { 0, 1, 2, 3 } },
    });
    std::vector<int> offsets = { 0 };
    for (PLYComponent& comp : components)
        offsets.push_back(offsets.back() + comp.n);

    fprintf(fp, "ply\n");
    fprintf(fp, "format binary_little_endian 1.0\n");
    fprintf(fp, "element vertex %d\n", (int)num_splats);
    for (PLYComponent& comp : components) {
        for (std::string name : comp.names)
            fprintf(fp, "property float %s\n", name.c_str());
    }
    fprintf(fp, "end_header\n");

    float buf[256];
    for (size_t si = 0; si < num_splats; si++) {
        for (int ci = 0; ci < (int)components.size(); ci++) {
            PLYComponent &comp = components[ci];
            int offset = offsets[ci];
            float* data = &comp.buf[si*comp.n];
            for (int i = 0; i < comp.n; i++)
                buf[offset+comp.permute[i]] = data[i];
        }
        fwrite(buf, sizeof(float), offsets.back(), fp);
    }

    fclose(fp);
}
