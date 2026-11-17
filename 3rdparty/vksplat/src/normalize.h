// Vendored from VkSplat (Apache-2.0), modified by osgVerse: glm -> Eigen.
// World-space normalization ported from VkSplat's colmap_reader.cpp, which follows
// nerfstudio/gsplat conventions.
#pragma once

#include <Eigen/Core>
#include <vector>

// Normalizes camera-to-world poses and sparse points in-place: aligns the world up
// axis, recenters the scene by camera focus points, rescales by median camera distance,
// aligns principal axes, and fixes upside-down orientation. Returns the accumulated
// similarity transform.
Eigen::Matrix4f normalizeWorldSpace(std::vector<Eigen::Matrix4f>& camtoworlds,
                                    std::vector<Eigen::Vector3f>& points,
                                    bool normalize = true);
