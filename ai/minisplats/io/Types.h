// mini/io/Types.h — Input/output data structures for the mini trainer.
//
// Input:  Eigen intrinsics/extrinsics + cv::Mat images (no COLMAP/OpenSfM parsing).
// Output: std::vector<Eigen::Vector3f> attribute arrays.
//
// Convention: viewmat is world-to-camera, OpenCV style (+Z forward, Y down).
// This matches what the mini projection code expects (pView.z > 0 for visible
// points). If your data is camera-to-world, invert it before filling viewmat.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_IO_TYPES_H
#define MINI_IO_TYPES_H

#include <Eigen/Dense>
#include <osg/Image>
#include <osg/ref_ptr>
#include <vector>

namespace mini { namespace io {

// ---- Input camera ----
struct Camera {
    // Intrinsics: [fx 0 cx; 0 fy cy; 0 0 1]
    Eigen::Matrix3f K = Eigen::Matrix3f::Identity();

    // World-to-camera extrinsic (OpenCV convention: +Z forward, Y down, X right).
    // Row-major 4x4.  viewmat = [R | t; 0 0 0 1]  with  p_cam = R @ p_world + t.
    Eigen::Matrix4f viewmat = Eigen::Matrix4f::Identity();

    // Image: RGB uint8, 8-bit.  Any origin flag works — imageToTensor()
    // normalizes rows so that tensor row 0 is the image top.
    osg::ref_ptr<osg::Image> image;
};

// ---- Input sparse point (for gaussian initialization) ----
struct Point {
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f color    = Eigen::Vector3f::Constant(0.5f); // RGB [0,1]
};

// ---- Input dataset ----
struct Dataset {
    std::vector<Camera> cameras;
    std::vector<Point>  points;
};

// ---- Output: trained gaussian attributes ----
// Each vector has one entry per gaussian.  means/scales/colors use Vector3f;
// rotations use Vector4f (quaternion [w, x, y, z], normalized);
// opacities are scalar in [0, 1].
struct GaussianOutput {
    std::vector<Eigen::Vector3f> means;
    std::vector<Eigen::Vector3f> scales;      // actual scales (exp'd, not log)
    std::vector<Eigen::Vector4f> rotations;   // unit quaternions [w, x, y, z]
    std::vector<Eigen::Vector3f> colors;      // RGB [0, 1] from SH DC term
    std::vector<float>           opacities;   // [0, 1]

    // Full SH coefficients (flattened [N, K, 3], row-major).
    // K = (shDegree+1)^2.  Empty if SH was not used.
    std::vector<float> shCoeffs;
    int shDegree = 0;

    size_t size() const { return means.size(); }
};

} } // namespace mini::io

#endif // MINI_IO_TYPES_H
