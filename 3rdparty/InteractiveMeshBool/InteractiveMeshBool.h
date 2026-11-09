#ifndef OSGVERSE_INTERACTIVEMESHBOOL_WRAPPER_H
#define OSGVERSE_INTERACTIVEMESHBOOL_WRAPPER_H

#include <cstdint>
#include <vector>

// Minimal non-inline wrapper around InteractiveMeshBool. The upstream sources are a
// header-style library (each .cpp is included by its header), so they cannot be compiled
// as separate translation units. InteractiveMeshBool.cpp includes all upstream .cpp files
// in one TU and exposes this small interface; osgVerse code only sees this header.

namespace osgVerse
{
namespace imb
{
    enum BooleanOp
    {
        Union = 0,
        Intersection = 1,
        Subtraction = 2,
        Xor = 3
    };

    // Both operands are indexed triangle meshes. inLabels has one entry per triangle and
    // must be 0 for the first operand and 1 for the second. Output coordinates are in the
    // same space as the input (world space if the caller baked transforms).
    void booleanPipeline(const std::vector<double>& inCoords, const std::vector<uint32_t>& inTris,
                         const std::vector<uint32_t>& inLabels, BooleanOp op,
                         std::vector<double>& outCoords, std::vector<uint32_t>& outTris);
}
}

#endif
