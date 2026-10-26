#include <osg/io_utils>
#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/TriangleIndexFunctor>
#include <algorithm>
#include <cstring>
#include <iterator>
#include "pipeline/Rasterizer.h"
#include "pipeline/Utilities.h"
#include "Utilities.h"

using namespace osgVerse;

namespace
{
    // Type-agnostic vertex array compaction: keeps elements whose remapping is valid
    osg::Array* compactArray(osg::Array* src, const std::vector<unsigned int>& remap, unsigned int newCount)
    {
        if (src == NULL || src->getNumElements() == 0) return src;
        osg::ref_ptr<osg::Array> dst = static_cast<osg::Array*>(src->clone(osg::CopyOp::SHALLOW_COPY));
        if (!dst.valid()) return src;

        dst->setBufferObject(NULL);  // The compacted array needs to be uploaded again
        dst->resizeArray(newCount);

        unsigned int elemSize = src->getElementSize();
        for (unsigned int i = 0; i < (unsigned int)remap.size(); ++i)
        {
            unsigned int ni = remap[i];
            if (ni == 0xFFFFFFFF || ni >= newCount) continue;

            const void* srcPtr = src->getDataPointer(i);
            void* dstPtr = const_cast<void*>(dst->getDataPointer(ni));
            if (srcPtr != NULL && dstPtr != NULL) memcpy(dstPtr, srcPtr, elemSize);
        }
        return dst.release();
    }

    // Collects triangles of a primitive set, skipping the removed ones
    struct TriangleHarvester
    {
        TriangleHarvester() : index(0), removed(NULL), indices(NULL) {}
        void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
        {
            if (removed->find(index) == removed->end())
            {
                indices->push_back(i1); indices->push_back(i2); indices->push_back(i3);
            }
            ++index;
        }

        unsigned int index;
        const std::set<unsigned int>* removed;
        std::vector<unsigned int>* indices;
    };
}

MeshInternalOptimizer::MeshInternalOptimizer(float angleStep, float horizontalStart,
                                             float verticalStart, float verticalRange)
{
    _angleStep = angleStep;
    _horizontalStart = horizontalStart; _horizontalRange = 360.0f;
    _verticalStart = verticalStart; _verticalRange = verticalRange;
    _fov = 60.0f; _distanceScale = 1.05f;
    _width = _height = 1024;
    _removedTriangles = _removedVertices = _numViews = 0;
}

MeshInternalOptimizer::~MeshInternalOptimizer()
{}

osg::Node* MeshInternalOptimizer::optimize(osg::Node* node)
{
    _removedTriangles = _removedVertices = _numViews = 0;
    if (node == NULL) return NULL;

    osg::BoundingSphere bs = node->getBound();
    if (!bs.valid())
    {
        OSG_WARN << "[MeshInternalOptimizer] Input model has an invalid bound" << std::endl;
        return node;
    }

    unsigned int width = std::max(_width & (~7u), 8u), height = std::max(_height & (~7u), 8u);
    std::vector<OrbitView> views = createOrbitViews(
        bs, _angleStep, _horizontalStart, _horizontalRange, _verticalStart, _verticalRange,
        _fov, 1.0f, _distanceScale);
    _numViews = (unsigned int)views.size();
    if (views.empty()) return node;

    // The model is used as its own occluder, so hidden interior structures can be found.
    // A single pass is not enough because quads are regrouped when vertices are compacted,
    // so the analysis is repeated until no more triangles can be removed (at most 4 passes).
    osg::ref_ptr<UserRasterizer> rasterizer = new UserRasterizer(width, height);
    typedef std::pair<unsigned int, unsigned int> TriangleKey;  // (primitive set, triangle index)
    for (int pass = 0; pass < 4; ++pass)
    {
        osg::ref_ptr<UserOccluder> occluder = new UserOccluder(*node);
        if (!occluder->hasTriangleReferences())
        {
            if (pass == 0)
                OSG_WARN << "[MeshInternalOptimizer] Triangle references are unavailable, the model "
                         << "can not be optimized (is the software rasterizer enabled?)" << std::endl;
            break;
        }

        std::map<osg::Geometry*, std::set<TriangleKey>> candidates;
        for (size_t i = 0; i < views.size(); ++i)
        {
            std::vector<TriangleReference> invisible;
            OcclusionAnalyzer::findInvisibleTriangles(
                *occluder, *rasterizer, views[i].view, views[i].proj, invisible);

            std::map<osg::Geometry*, std::set<TriangleKey>> current;
            for (size_t j = 0; j < invisible.size(); ++j)
            {
                const TriangleReference& ref = invisible[j];
                if (!ref.geometry.valid()) continue;
                current[ref.geometry.get()].insert(TriangleKey(ref.primitiveSet, ref.primitiveIndex));
            }

            if (i == 0) candidates.swap(current);
            else
            {
                // Only triangles proven invisible in all sampled views may be removed
                for (std::map<osg::Geometry*, std::set<TriangleKey>>::iterator it = candidates.begin();
                     it != candidates.end(); )
                {
                    std::map<osg::Geometry*, std::set<TriangleKey>>::iterator it2 = current.find(it->first);
                    if (it2 == current.end()) { it = candidates.erase(it); continue; }

                    std::set<TriangleKey> merged;
                    std::set_intersection(it->second.begin(), it->second.end(),
                                          it2->second.begin(), it2->second.end(),
                                          std::inserter(merged, merged.begin()));
                    if (merged.empty()) it = candidates.erase(it);
                    else { it->second.swap(merged); ++it; }
                }
            }
            if (candidates.empty()) break;
        }

        unsigned int numRemoved = 0;
        for (std::map<osg::Geometry*, std::set<TriangleKey>>::iterator it = candidates.begin();
             it != candidates.end(); ++it)
        {
            std::map<unsigned int, std::set<unsigned int>> removals;
            for (std::set<TriangleKey>::iterator k = it->second.begin(); k != it->second.end(); ++k)
                removals[k->first].insert(k->second);

            numRemoved += (unsigned int)it->second.size();
            removeTriangles(*it->first, removals);
        }
        _removedTriangles += numRemoved;
        if (numRemoved == 0) break;
    }
    return node;
}

void MeshInternalOptimizer::removeTriangles(osg::Geometry& geom,
                                            const std::map<unsigned int, std::set<unsigned int>>& removals)
{
    osg::Array* vertexArray = geom.getVertexArray();
    if (vertexArray == NULL || removals.empty()) return;
    unsigned int oldVertexCount = vertexArray->getNumElements();

    // Rebuild the index data of each primitive set, removing the wanted triangles
    struct PrimitiveData { GLenum mode; std::vector<unsigned int> indices; };
    std::vector<PrimitiveData> primitives;
    for (unsigned int p = 0; p < geom.getNumPrimitiveSets(); ++p)
    {
        osg::PrimitiveSet* ps = geom.getPrimitiveSet(p);
        PrimitiveData data; data.mode = (ps != NULL) ? ps->getMode() : GL_TRIANGLES;
        if (ps != NULL)
        {
            std::map<unsigned int, std::set<unsigned int>>::const_iterator it = removals.find(p);
            if (it != removals.end())
            {
                osg::TriangleIndexFunctor<TriangleHarvester> functor;
                TriangleHarvester& op = functor;
                op.removed = &it->second; op.indices = &data.indices;
                ps->accept(functor);
                data.mode = GL_TRIANGLES;  // non-triangle modes are triangulated when editing
            }
            else
            {
                data.indices.reserve(ps->getNumIndices());
                for (unsigned int i = 0; i < ps->getNumIndices(); ++i)
                    data.indices.push_back(ps->index(i));
            }
        }
        primitives.push_back(data);
    }

    // Find vertices which are still used, then build the old-to-new index remapping
    std::vector<unsigned char> used(oldVertexCount, 0);
    for (size_t p = 0; p < primitives.size(); ++p)
    {
        std::vector<unsigned int>& ids = primitives[p].indices;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] < oldVertexCount) used[ids[i]] = 1;
    }

    std::vector<unsigned int> remap(oldVertexCount, 0xFFFFFFFF);
    unsigned int newVertexCount = 0;
    for (unsigned int i = 0; i < oldVertexCount; ++i)
        if (used[i]) remap[i] = newVertexCount++;

    bool toCompact = (newVertexCount < oldVertexCount);
    if (toCompact)
    {
        for (size_t p = 0; p < primitives.size(); ++p)
        {
            std::vector<unsigned int>& ids = primitives[p].indices;
            for (size_t i = 0; i < ids.size(); ++i)
                if (ids[i] < oldVertexCount) ids[i] = remap[ids[i]];
        }

        // Compact all arrays which are indexed by vertices, keeping their types and bindings
        auto compactIfPerVertex = [&](osg::Array* arr) -> osg::Array*
        {
            if (arr == NULL || arr->getNumElements() != oldVertexCount) return arr;
            return compactArray(arr, remap, newVertexCount);
        };

        geom.setVertexArray(compactIfPerVertex(vertexArray));

        osg::Array* oldArray = geom.getNormalArray();
        if (oldArray != NULL)
            geom.setNormalArray(compactIfPerVertex(oldArray), oldArray->getBinding());

        oldArray = geom.getColorArray();
        if (oldArray != NULL)
            geom.setColorArray(compactIfPerVertex(oldArray), oldArray->getBinding());

        oldArray = geom.getSecondaryColorArray();
        if (oldArray != NULL)
            geom.setSecondaryColorArray(compactIfPerVertex(oldArray), oldArray->getBinding());

        oldArray = geom.getFogCoordArray();
        if (oldArray != NULL)
            geom.setFogCoordArray(compactIfPerVertex(oldArray), oldArray->getBinding());

        osg::Geometry::ArrayList& texCoords = geom.getTexCoordArrayList();
        for (size_t i = 0; i < texCoords.size(); ++i)
            texCoords[i] = compactIfPerVertex(texCoords[i].get());

        osg::Geometry::ArrayList& attribs = geom.getVertexAttribArrayList();
        for (size_t i = 0; i < attribs.size(); ++i)
            attribs[i] = compactIfPerVertex(attribs[i].get());

        _removedVertices += (oldVertexCount - newVertexCount);
    }

    // Apply the new primitive sets (only changed ones if vertices are not compacted)
    for (size_t p = 0; p < primitives.size(); ++p)
    {
        bool changed = (removals.find((unsigned int)p) != removals.end());
        if (!toCompact && !changed) continue;

        osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(primitives[p].mode);
        de->assign(primitives[p].indices.begin(), primitives[p].indices.end());
        geom.setPrimitiveSet((unsigned int)p, de.get());
    }
    geom.dirtyBound();
}
