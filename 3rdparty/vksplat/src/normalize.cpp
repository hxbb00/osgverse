// Vendored from VkSplat (Apache-2.0), modified by osgVerse: glm -> Eigen.
#include "normalize.h"

#include <Eigen/Eigenvalues>
#include <algorithm>
#include <numeric>
#include <string>


namespace {

float median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    if (n % 2 == 0)
        return (values[n / 2 - 1] + values[n / 2]) / 2.0f;
    return values[n / 2];
}

Eigen::Vector3f medianVec3(const std::vector<Eigen::Vector3f>& vectors) {
    if (vectors.empty()) return Eigen::Vector3f::Zero();
    std::vector<float> x_vals, y_vals, z_vals;
    x_vals.reserve(vectors.size());
    y_vals.reserve(vectors.size());
    z_vals.reserve(vectors.size());
    for (const auto& v : vectors) {
        x_vals.push_back(v.x());
        y_vals.push_back(v.y());
        z_vals.push_back(v.z());
    }
    return Eigen::Vector3f(median(x_vals), median(y_vals), median(z_vals));
}

std::vector<Eigen::Matrix4f> transformCameras(const Eigen::Matrix4f& matrix,
                                              const std::vector<Eigen::Matrix4f>& camtoworlds) {
    std::vector<Eigen::Matrix4f> result;
    result.reserve(camtoworlds.size());
    for (const auto& cam : camtoworlds) {
        Eigen::Matrix4f transformed = matrix * cam;
        // Normalize the rotation part (first 3 columns should have unit length)
        float scaling = transformed.block<3, 1>(0, 0).norm();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                transformed(i, j) /= scaling;
        result.push_back(transformed);
    }
    return result;
}

std::vector<Eigen::Vector3f> transformPoints(const Eigen::Matrix4f& matrix,
                                             const std::vector<Eigen::Vector3f>& points) {
    std::vector<Eigen::Vector3f> result;
    result.reserve(points.size());
    Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    Eigen::Vector3f translation = matrix.block<3, 1>(0, 3);
    for (const auto& point : points)
        result.push_back(rotation * point + translation);
    return result;
}

Eigen::Matrix4f similarityFromCameras(const std::vector<Eigen::Matrix4f>& c2w,
                                      bool strict_scaling = false,
                                      const std::string& center_method = "focus") {
    if (c2w.empty()) return Eigen::Matrix4f::Identity();

    std::vector<Eigen::Vector3f> poss, ups, fwds;
    poss.reserve(c2w.size()); ups.reserve(c2w.size()); fwds.reserve(c2w.size());
    for (const auto& cam : c2w) {
        poss.push_back(cam.block<3, 1>(0, 3));
        Eigen::Matrix3f rotation = cam.block<3, 3>(0, 0);
        ups.push_back(rotation * Eigen::Vector3f(0.0f, -1.0f, 0.0f));
        fwds.push_back(rotation * Eigen::Vector3f(0.0f, 0.0f, 1.0f));
    }

    // (1) Estimate world up direction
    Eigen::Vector3f world_up = Eigen::Vector3f::Zero();
    for (const auto& up : ups) world_up += up;
    world_up /= static_cast<float>(ups.size());
    world_up.normalize();

    // Align world up with camera space up (0, -1, 0)
    Eigen::Vector3f up_camspace(0.0f, -1.0f, 0.0f);
    float c = up_camspace.dot(world_up);
    Eigen::Vector3f cross_product = world_up.cross(up_camspace);

    Eigen::Matrix3f R_align;
    if (c > -1.0f) {
        Eigen::Matrix3f skew;
        skew << 0.0f, -cross_product.z(), cross_product.y(),
                cross_product.z(), 0.0f, -cross_product.x(),
                -cross_product.y(), cross_product.x(), 0.0f;
        R_align = Eigen::Matrix3f::Identity() + skew + (skew * skew) * (1.0f / (1.0f + c));
    } else {
        // 180-degree rotation about x-axis
        R_align = Eigen::Matrix3f::Identity();
        R_align(0, 0) = -1.0f;
    }

    // Apply rotation to positions and forwards
    for (auto& pos : poss) pos = R_align * pos;
    for (auto& fwd : fwds) fwd = R_align * fwd;

    // (2) Recenter the scene
    Eigen::Vector3f translate;
    if (center_method == "focus") {
        std::vector<Eigen::Vector3f> nearest_points;
        nearest_points.reserve(poss.size());
        for (size_t i = 0; i < poss.size(); ++i)
            nearest_points.push_back(poss[i] + (-poss[i]).dot(fwds[i]) * fwds[i]);
        translate = -medianVec3(nearest_points);
    } else if (center_method == "poses") {
        translate = -medianVec3(poss);
    } else {
        translate = Eigen::Vector3f::Zero();
    }

    // (3) Rescale the scene
    std::vector<float> distances;
    distances.reserve(poss.size());
    for (const auto& pos : poss)
        distances.push_back((pos + translate).norm());

    float scale;
    if (strict_scaling)
        scale = 1.0f / *std::max_element(distances.begin(), distances.end());
    else
        scale = 1.0f / median(distances);

    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform.block<3, 3>(0, 0) = R_align * scale;
    transform.block<3, 1>(0, 3) = translate * scale;
    return transform;
}

Eigen::Matrix4f alignPrincipalAxes(const std::vector<Eigen::Vector3f>& point_cloud) {
    if (point_cloud.empty()) return Eigen::Matrix4f::Identity();

    Eigen::Vector3f centroid = medianVec3(point_cloud);

    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const auto& point : point_cloud) {
        Eigen::Vector3d translated_point = (point - centroid).cast<double>();
        mean += translated_point;
        covariance += translated_point * translated_point.transpose();
    }
    covariance /= double(point_cloud.size() - 1);
    mean /= double(point_cloud.size());
    covariance -= mean * mean.transpose();

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    Eigen::Matrix3d eigenvectors = solver.eigenvectors();
    Eigen::Vector3d eigenvalues = solver.eigenvalues();

    // Sort eigenvectors by eigenvalues (descending order)
    std::vector<std::pair<float, int>> eigen_pairs;
    for (int i = 0; i < 3; ++i)
        eigen_pairs.push_back({ (float)eigenvalues[i], i });
    std::sort(eigen_pairs.rbegin(), eigen_pairs.rend());

    Eigen::Matrix3d sorted_eigenvectors;
    for (int i = 0; i < 3; ++i) {
        int idx = eigen_pairs[i].second;
        sorted_eigenvectors.col(i) = eigenvectors.col(idx);
    }
    eigenvectors = sorted_eigenvectors;

    // Check orientation - ensure right-handed coordinate system
    if (eigenvectors.determinant() < 0)
        eigenvectors.col(0) = -eigenvectors.col(0);

    Eigen::Matrix3f rotation_matrix = eigenvectors.transpose().cast<float>();
    Eigen::Vector3f new_translation = -rotation_matrix * centroid;

    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform.block<3, 3>(0, 0) = rotation_matrix;
    transform.block<3, 1>(0, 3) = new_translation;
    return transform;
}

}  // namespace


Eigen::Matrix4f normalizeWorldSpace(std::vector<Eigen::Matrix4f>& camtoworlds,
                                    std::vector<Eigen::Vector3f>& points,
                                    bool normalize) {
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    if (!normalize) return transform;

    // Step 1: Similarity transform from cameras
    Eigen::Matrix4f T1 = similarityFromCameras(camtoworlds);
    camtoworlds = transformCameras(T1, camtoworlds);
    points = transformPoints(T1, points);

    // Step 2: Align principal axes
    Eigen::Matrix4f T2 = alignPrincipalAxes(points);
    camtoworlds = transformCameras(T2, camtoworlds);
    points = transformPoints(T2, points);
    transform = T2 * T1;

    // Step 3: Fix for upside down
    std::vector<float> z_coords;
    z_coords.reserve(points.size());
    for (const auto& point : points) z_coords.push_back(point.z());
    float z_median = median(z_coords);
    float z_mean = std::accumulate(z_coords.begin(), z_coords.end(), 0.0f) / z_coords.size();
    if (z_median > z_mean) {
        Eigen::Matrix4f T3 = Eigen::Matrix4f::Identity();
        T3(1, 1) = -1.0f;  // flip y
        T3(2, 2) = -1.0f;  // flip z
        camtoworlds = transformCameras(T3, camtoworlds);
        points = transformPoints(T3, points);
        transform = T3 * transform;
    }
    return transform;
}
