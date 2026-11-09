// SPDX-License-Identifier: MIT
//
// Single-translation-unit build for InteractiveMeshBool.
//
// Upstream is a header-style library: every .cpp file is meant to be included by its header
// (see the commented "#include \"*.cpp\"" at the bottom of each header), and all functions are
// declared "inline". Compiling the .cpp files separately leaves the cross-file inline symbols
// undefined, so they are all included here in dependency order instead.

#include "processing.cpp"
#include "aux_structure.cpp"
#include "fast_trimesh.cpp"
#include "triangle_soup.cpp"
#include "intersection_classification.cpp"
#include "solve_intersections.cpp"
#include "triangulation.cpp"
#include "foctree.cpp"
#include "booleans.cpp"

#include "InteractiveMeshBool.h"

namespace osgVerse
{
namespace imb
{
    void booleanPipeline(const std::vector<double>& inCoords, const std::vector<uint32_t>& inTris,
                         const std::vector<uint32_t>& inLabels, BooleanOp op,
                         std::vector<double>& outCoords, std::vector<uint32_t>& outTris)
    {
        static_assert((int)Union == (int)::UNION && (int)Intersection == (int)::INTERSECTION &&
                      (int)Subtraction == (int)::SUBTRACTION && (int)Xor == (int)::XOR,
                      "InteractiveMeshBool enum values changed");

        std::vector<std::bitset<NBIT>> outLabels;
        ::booleanPipeline(inCoords, inTris, inLabels, static_cast<::BoolOp>(op),
                          outCoords, outTris, outLabels);
    }
}
}
