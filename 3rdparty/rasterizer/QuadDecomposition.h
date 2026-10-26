#pragma once

#ifndef _MSC_VER
#   include <cstdint>
#endif
#include <vector>
#include <xmmintrin.h>

class QuadDecomposition
{
public:
  // decomposes triangles into quads (4 indices per quad). If quadSources is not null, it receives
  // the source triangle index (or indices) of each quad: 2 entries per quad, the second one is
  // 0xFFFFFFFF when the quad comes from a single triangle
  static std::vector<uint32_t> decompose(const std::vector<uint32_t>& indices, const std::vector<__m128>& vertices,
                                        std::vector<uint32_t>* quadSources = nullptr);
};


