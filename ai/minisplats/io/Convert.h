// mini/io/Convert.h — Conversions between Eigen / osg::Image and mini::Tensor.
//
// SPDX-License-Identifier: MIT
#ifndef MINI_IO_CONVERT_H
#define MINI_IO_CONVERT_H

#include "../Tensor.h"
#include "Types.h"
#include <Eigen/Dense>
#include <osg/Image>

#include <stdexcept>

namespace mini { namespace io {

// ---- Eigen → Tensor ----

inline Tensor toTensor(const Eigen::Matrix4f& m) {
    // Eigen is column-major by default; we copy element-wise to get row-major.
    float data[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            data[r * 4 + c] = m(r, c);
    return Tensor::from_blob(data, {4, 4});
}

inline Tensor toTensor(const Eigen::Matrix3f& m) {
    float data[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            data[r * 3 + c] = m(r, c);
    return Tensor::from_blob(data, {3, 3});
}

inline Tensor toTensor(const Eigen::Vector3f& v) {
    return Tensor::from_blob(v.data(), {3});
}

// ---- Tensor → Eigen ----

inline Eigen::Vector3f toVector3f(const Tensor& t, int row = 0) {
    const float* p = t.data_ptr() + row * 3;
    return Eigen::Vector3f(p[0], p[1], p[2]);
}

inline Eigen::Vector4f toVector4f(const Tensor& t, int row = 0) {
    const float* p = t.data_ptr() + row * 4;
    return Eigen::Vector4f(p[0], p[1], p[2], p[3]);
}

// ---- osg::Image → Tensor ----
// Supported: GL_RGB / GL_RGBA / GL_BGRA / GL_LUMINANCE with GL_UNSIGNED_BYTE.
// Tensor row 0 is the image top.  osg::Image defaults to BOTTOM_LEFT origin
// (row 0 = image bottom), so rows are flipped automatically in that case.
// Tensor is RGB float [H, W, 3] in [0, 1].
inline Tensor imageToTensor(const osg::Image& img) {
    if (img.getDataType() != GL_UNSIGNED_BYTE)
        throw std::runtime_error("imageToTensor: only GL_UNSIGNED_BYTE supported");
    if (!img.data())
        throw std::runtime_error("imageToTensor: image has no data");

    const int W = img.s(), H = img.t();
    const unsigned int fmt = img.getPixelFormat();
    const bool flip = (img.getOrigin() == osg::Image::BOTTOM_LEFT);
    auto t = Tensor::empty({H, W, 3});
    float* out = t.data_ptr();

    for (int y = 0; y < H; y++) {
        const unsigned char* row = img.data(0, y);
        float* outRow = out + (size_t)(flip ? H - 1 - y : y) * W * 3;
        switch (fmt) {
        case GL_RGB:
            for (int x = 0; x < W; x++) {
                outRow[x*3+0] = row[x*3+0] / 255.0f;
                outRow[x*3+1] = row[x*3+1] / 255.0f;
                outRow[x*3+2] = row[x*3+2] / 255.0f;
            }
            break;
        case GL_RGBA:
        case GL_BGRA:
            for (int x = 0; x < W; x++) {
                const bool bgra = (fmt == GL_BGRA);
                outRow[x*3+0] = row[x*4 + (bgra ? 2 : 0)] / 255.0f;
                outRow[x*3+1] = row[x*4 + 1] / 255.0f;
                outRow[x*3+2] = row[x*4 + (bgra ? 0 : 2)] / 255.0f;
            }
            break;
        case GL_LUMINANCE:
            for (int x = 0; x < W; x++) {
                const float v = row[x] / 255.0f;
                outRow[x*3+0] = v;
                outRow[x*3+1] = v;
                outRow[x*3+2] = v;
            }
            break;
        default:
            throw std::runtime_error("imageToTensor: unsupported pixel format");
        }
    }
    return t;
}

// ---- Tensor → osg::Image ----
// Tensor is RGB float [H, W, 3] in [0, 1].  Returns RGB uint8 osg::Image
// with TOP_LEFT origin (row 0 = image top; caller owns via ref_ptr).
inline osg::ref_ptr<osg::Image> tensorToImage(const Tensor& t) {
    int H = (int)t.size(0), W = (int)t.size(1);
    auto img = new osg::Image;
    img->allocateImage(W, H, 1, GL_RGB, GL_UNSIGNED_BYTE);
    img->setOrigin(osg::Image::TOP_LEFT);
    const float* in = t.data_ptr();
    for (int y = 0; y < H; y++) {
        unsigned char* row = img->data(0, y);
        const float* inRow = in + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                float v = inRow[x*3+c] * 255.0f;
                v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
                row[x*3+c] = (unsigned char)(v + 0.5f);
            }
        }
    }
    return img;
}

// ---- Extract camera position from viewmat ----
// viewmat = [R | t; 0 0 0 1].  Camera position in world = -R^T @ t.
inline Eigen::Vector3f cameraPosition(const Eigen::Matrix4f& viewmat) {
    Eigen::Matrix3f R = viewmat.topLeftCorner<3, 3>();
    Eigen::Vector3f t = viewmat.topRightCorner<3, 1>();
    return -R.transpose() * t;
}

} }// namespace mini::io

#endif // MINI_IO_CONVERT_H
