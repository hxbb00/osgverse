#ifdef VERSE_ENABLE_RASTERIZER
#   include <rasterizer/VectorMath.h>
#   include <rasterizer/QuadDecomposition.h>
#   include <rasterizer/SurfaceAreaHeuristic.h>
#   include <rasterizer/Occluder.h>
#   include <rasterizer/Rasterizer.h>
#   include <osg/TriangleIndexFunctor>
#   include <osg/PrimitiveSet>
#endif
#include <osgDB/WriteFile>
#include <algorithm>
#include "modeling/Utilities.h"
#include "Rasterizer.h"

namespace osgVerse
{
#ifdef VERSE_ENABLE_RASTERIZER
    static __m128 convertFromVec4(const osg::Vec4& v)
    { __m128 vec = _mm_load_ps(v.ptr()); return vec; }

    static __m128 convertFromVec3(const osg::Vec3& v)
    { osg::Vec4 v4(v, 1.0f); return convertFromVec4(v4); }

    static osg::Vec4 convertToVec4(__m128 vec)
    { osg::Vec4 v; _mm_store_ps(v.ptr(), vec); return v; }

    static osg::Vec3 convertToVec3(__m128 vec)
    { osg::Vec4 v = convertToVec4(vec); return osg::Vec3(v[0], v[1], v[2]); }

    struct BatchOccluderData { std::unique_ptr<Occluder> data; };
    struct UserRasterizerData { std::unique_ptr<Rasterizer> data; };

    /** Collects triangles together with their source references. Vertices are welded and
        transformed into world space exactly like MeshCollector does, but each emitted triangle
        also records which Geometry / PrimitiveSet / triangle index it comes from */
    class TriangleRefCollector : public MeshCollector
    {
    public:
        std::vector<TriangleReference>& getReferences() { return _refs; }

        virtual void apply(osg::Geometry& geom)
        {
            osg::Vec3Array* varr = dynamic_cast<osg::Vec3Array*>(geom.getVertexArray());
            if (varr)
            {
                osg::Matrix matrix;
                if (!_matrixStack.empty()) matrix = _matrixStack.back();
                if (!_globalVertices) _vertexMap.clear();

                for (unsigned int i = 0; i < geom.getNumPrimitiveSets(); ++i)
                {
                    osg::PrimitiveSet* pset = geom.getPrimitiveSet(i);
                    if (pset == NULL) continue;

                    osg::TriangleIndexFunctor<WeldRefOperator> functor;
                    WeldRefOperator& op = functor;
                    op.inputV = varr; op.matrix = matrix;
                    op.vertices = &_vertices; op.indices = &_indices;
                    op.vertexMap = _weldVertices ? &_vertexMap : NULL;
                    op.refs = &_refs; op.geometry = &geom; op.primitiveSet = i;
                    pset->accept(functor);
                }
            }
#if OSG_VERSION_GREATER_THAN(3, 4, 1)
            traverse(geom);
#endif
        }

    protected:
        struct WeldRefOperator
        {
            WeldRefOperator()
            :   inputV(NULL), vertices(NULL), indices(NULL), vertexMap(NULL),
                refs(NULL), geometry(NULL), primitiveSet(0), triangleIndex(0) {}

            inline unsigned int getVertexIndex(const osg::Vec3& v)
            {
                if (vertexMap)
                {
                    std::map<osg::Vec3, unsigned int, Vec3MapComparer>::iterator it = vertexMap->find(v);
                    if (it != vertexMap->end()) return it->second;

                    const float eps = 1e-4f, epsSq = eps * eps;
                    osg::Vec3 key = v;
                    it = vertexMap->lower_bound(osg::Vec3(v.x() - eps, -FLT_MAX, -FLT_MAX));
                    for (; it != vertexMap->end(); ++it)
                    {
                        const osg::Vec3& k = it->first;
                        if (k.x() > v.x() + eps) break;
                        if ((k - v).length2() < epsSq) { key = k; break; }
                    }

                    unsigned int index = (unsigned int)vertices->size();
                    (*vertexMap)[key] = index; vertices->push_back(key);
                    return index;
                }

                unsigned int index = (unsigned int)vertices->size();
                vertices->push_back(v);
                return index;
            }

            inline void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
            {
                unsigned int triangle = triangleIndex++;
                unsigned int n1 = getVertexIndex((*inputV)[i1] * matrix);
                unsigned int n2 = getVertexIndex((*inputV)[i2] * matrix);
                unsigned int n3 = getVertexIndex((*inputV)[i3] * matrix);
                if (n1 == n2 || n2 == n3 || n1 == n3) return;  // same as MeshCollector

                indices->push_back(n1); indices->push_back(n2); indices->push_back(n3);
                refs->push_back(TriangleReference(geometry, primitiveSet, triangle));
            }

            osg::Vec3Array* inputV;
            osg::Matrix matrix;
            std::vector<osg::Vec3>* vertices;
            std::vector<unsigned int>* indices;
            std::map<osg::Vec3, unsigned int, Vec3MapComparer>* vertexMap;
            std::vector<TriangleReference>* refs;
            osg::Geometry* geometry;
            unsigned int primitiveSet, triangleIndex;
        };

        std::vector<TriangleReference> _refs;
    };

    BatchOccluder::BatchOccluder(UserOccluder* u, const std::vector<osg::Vec3> vertices,
                                 const osg::BoundingBoxf& refBound) : _owner(u)
    {
        std::vector<__m128> vList;
        for (size_t i = 0; i < vertices.size(); ++i)
            vList.push_back(convertFromVec3(vertices[i]));
        __m128 refMin = convertFromVec3(refBound._min);
        __m128 refMax = convertFromVec3(refBound._max);

        BatchOccluderData* od = new BatchOccluderData;
        od->data = Occluder::bake(vList, refMin, refMax);
        _privateData = od; _numVertices = vertices.size();
    }

    BatchOccluder::BatchOccluder(UserOccluder* u, void* verticesInternal,
                                 const osg::BoundingBoxf& refBound) : _owner(u)
    {
        std::vector<__m128>* vList = (std::vector<__m128>*)verticesInternal;
        __m128 refMin = convertFromVec3(refBound._min);
        __m128 refMax = convertFromVec3(refBound._max);

        BatchOccluderData* od = new BatchOccluderData;
        od->data = Occluder::bake(*vList, refMin, refMax);
        _privateData = od; _numVertices = vList->size();
    }

    BatchOccluder::~BatchOccluder()
    { BatchOccluderData* od = (BatchOccluderData*)_privateData; delete od; }

    osg::BoundingBoxf BatchOccluder::getBound() const
    {
        BatchOccluderData* od = (BatchOccluderData*)_privateData;
        osg::BoundingBoxf bb(convertToVec3(od->data->m_boundsMin),
                             convertToVec3(od->data->m_boundsMax)); return bb;
    }

    osg::Vec3 BatchOccluder::getCenter() const
    {
        BatchOccluderData* od = (BatchOccluderData*)_privateData;
        return convertToVec3(od->data->m_center);
    }

    UserOccluder::UserOccluder(osg::Geometry& geom)
    {
        TriangleRefCollector collector;
        collector.setWeldingVertices(true);
        collector.setUseGlobalVertices(true);
        _name = geom.getName(); collector.apply(geom);
        set(collector.getVertices(), collector.getTriangles(), &collector.getReferences());
    }

    UserOccluder::UserOccluder(osg::Node& node)
    {
        TriangleRefCollector collector;
        collector.setWeldingVertices(true);
        collector.setUseGlobalVertices(true);
        _name = node.getName(); node.accept(collector);
        set(collector.getVertices(), collector.getTriangles(), &collector.getReferences());
    }

    UserOccluder::UserOccluder(const std::string& name, const std::vector<osg::Vec3> vertices,
                               const std::vector<unsigned int>& indices) : _name(name)
    { set(vertices, indices); }

#ifdef WITH_SIMD_INPUT
    UserOccluder::UserOccluder(const std::string& name, const std::vector<__m128> vertices,
                               const std::vector<unsigned int>& indices0) : _name(name)
    {
        std::vector<unsigned int> indices(indices0);
        indices = QuadDecomposition::decompose(indices, vertices);
        while (indices.size() % 32 != 0) indices.push_back(indices.back());  // Pad to a multiple of 8 quads

        std::vector<Aabb> quadAabbs;
        for (size_t j = 0; j < indices.size() / 4; ++j)
        {
            Aabb aabb; size_t q = j * 4;
            aabb.include(vertices[indices[q + 0]]);
            aabb.include(vertices[indices[q + 1]]);
            aabb.include(vertices[indices[q + 2]]);
            aabb.include(vertices[indices[q + 3]]);
            quadAabbs.push_back(aabb);
        }

        std::vector<std::vector<uint32_t>> batches = SurfaceAreaHeuristic::generateBatches(quadAabbs, 512, 8);
        osg::BoundingBoxf bb; for (size_t i = 0; i < vertices.size(); ++i) bb.expandBy(convertToVec3(vertices[i]));
        for (size_t i = 0; i < batches.size(); ++i)
        {
            std::vector<uint32_t>& batch = batches[i];
            std::vector<__m128> batchVertices;
            for (size_t j = 0; j < batch.size(); ++j)
            {
                uint32_t q = batch[j] * 4;
                batchVertices.push_back(vertices[indices[q + 0]]);
                batchVertices.push_back(vertices[indices[q + 1]]);
                batchVertices.push_back(vertices[indices[q + 2]]);
                batchVertices.push_back(vertices[indices[q + 3]]);
            }
            _batches.insert(new BatchOccluder(this, &batchVertices, bb));
            std::cout << "BATCH = " << batchVertices.size() << "\n";
        }
    }
#endif

    void UserOccluder::set(const std::vector<osg::Vec3> vertices0, const std::vector<unsigned int>& indices0,
                           const std::vector<TriangleReference>* triRefs)
    {
        std::vector<unsigned int> indices(indices0); std::vector<__m128> vertices;
        _triangles.clear(); _quads.clear();
        if (vertices0.empty() || indices0.empty())
        { OSG_WARN << "[UserOccluder] Empty vertex or index data" << std::endl; return; }

        for (size_t i = 0; i < vertices0.size(); ++i)
            vertices.push_back(convertFromVec3(vertices0[i]));

        std::vector<uint32_t> quadSources;
        indices = QuadDecomposition::decompose(indices, vertices, &quadSources);
        while (indices.size() % 32 != 0) indices.push_back(indices.back());  // Pad to a multiple of 8 quads

        // Keep source triangle references, but only when they match the collected triangles
        if (triRefs)
        {
            if (triRefs->size() == indices0.size() / 3) _triangles = *triRefs;
            else
                OSG_WARN << "[UserOccluder] Triangle references (" << triRefs->size()
                         << ") do not match collected triangles (" << indices0.size() / 3
                         << "), occluded triangles can not be reported" << std::endl;
        }

        // Record quad bounds and their source triangles for occlusion queries
        size_t quadCount = quadSources.size() / 2;
        _quads.resize(quadCount);
        for (size_t q = 0; q < quadCount; ++q)
        {
            QuadInfo& info = _quads[q];
            for (int k = 0; k < 4; ++k)
                info.bounds.expandBy(convertToVec3(vertices[indices[4 * q + k]]));
            info.triangles[0] = quadSources[2 * q + 0];
            info.triangles[1] = quadSources[2 * q + 1];
        }

        std::vector<Aabb> quadAabbs;
        for (size_t j = 0; j < indices.size() / 4; ++j)
        {
            Aabb aabb; size_t q = j * 4;
            aabb.include(vertices[indices[q + 0]]);
            aabb.include(vertices[indices[q + 1]]);
            aabb.include(vertices[indices[q + 2]]);
            aabb.include(vertices[indices[q + 3]]);
            quadAabbs.push_back(aabb);
        }

        std::vector<std::vector<uint32_t>> batches = SurfaceAreaHeuristic::generateBatches(quadAabbs, 512, 8);
        osg::BoundingBoxf bb; for (size_t i = 0; i < vertices0.size(); ++i) bb.expandBy(vertices0[i]);
        for (size_t i = 0; i < batches.size(); ++i)
        {
            std::vector<uint32_t>& batch = batches[i];
            std::vector<__m128> batchVertices;
            for (size_t j = 0; j < batch.size(); ++j)
            {
                uint32_t q = batch[j] * 4;
                batchVertices.push_back(vertices[indices[q + 0]]);
                batchVertices.push_back(vertices[indices[q + 1]]);
                batchVertices.push_back(vertices[indices[q + 2]]);
                batchVertices.push_back(vertices[indices[q + 3]]);
            }
            _batches.insert(new BatchOccluder(this, &batchVertices, bb));
        }
    }

    UserRasterizer::UserRasterizer(unsigned int w, unsigned int h)
    {
        UserRasterizerData* rd = new UserRasterizerData;
        rd->data = std::make_unique<Rasterizer>(w, h);
        _privateData = rd; _blockNumX = w / 8; _blockNumY = h / 8;
    }

    UserRasterizer::~UserRasterizer()
    { UserRasterizerData* rd = (UserRasterizerData*)_privateData; delete rd; }

    void UserRasterizer::setModelViewProjection(const osg::Matrix& view0, const osg::Matrix& proj0)
    {
        osg::Matrix convV(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        osg::Matrix convP(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, -0.5f, 0.0f, 0.0f, 0.0f, -0.5f, 1.0f);
        osg::Matrix view = view0 * convV, proj = proj0 * convP;  // FIXME: not consider non-perspective matrix
        proj(2, 3) = -proj(2, 3); proj(3, 2) = -proj(3, 2);      // http://perry.cz/articles/ProjectionMatrix.xhtml

        UserRasterizerData* rd = (UserRasterizerData*)_privateData;
        rd->data->setModelViewProjection(osg::Matrixf(view * proj).ptr());
    }

    void UserRasterizer::render(const osg::Vec3& cameraPos, std::vector<float>* depthData,
                                std::vector<unsigned short>* hizData)
    {
        std::vector<BatchOccluder*> globalOccluders;
        for (std::set<osg::ref_ptr<UserOccluder>>::iterator it = _occluders.begin();
             it != _occluders.end(); ++it)
        {
            std::set<osg::ref_ptr<BatchOccluder>>& batches = (*it)->getBatches();
            globalOccluders.insert(globalOccluders.end(), batches.begin(), batches.end());
        }

        UserRasterizerData* rd = (UserRasterizerData*)_privateData;
        rd->data->clear();

        // Sort front to back
        __m128 camPos = convertFromVec3(cameraPos);
        std::sort(globalOccluders.begin(), globalOccluders.end(), [&](const BatchOccluder* o1, const BatchOccluder* o2)
            {
                __m128 dist1 = _mm_sub_ps(((BatchOccluderData*)o1->getOccluder())->data->m_center, camPos);
                __m128 dist2 = _mm_sub_ps(((BatchOccluderData*)o2->getOccluder())->data->m_center, camPos);
                return _mm_comilt_ss(_mm_dp_ps(dist1, dist1, 0x7f), _mm_dp_ps(dist2, dist2, 0x7f));
            });

        // Rasterize all occluders
        for (size_t i = 0; i < globalOccluders.size(); ++i)
        {
            BatchOccluder* bo = globalOccluders[i]; bool needsClipping = false;
            BatchOccluderData* od = (BatchOccluderData*)bo->getOccluder();
            if (rd->data->queryVisibility(od->data->m_boundsMin, od->data->m_boundsMax, needsClipping))
            {
                if (needsClipping) rd->data->rasterize<true>(*od->data);
                else rd->data->rasterize<false>(*od->data);
            }   
        }

        // Get result depth image
        std::vector<__m128i>& depthBuffer = rd->data->getDepthBuffer();
        std::vector<uint16_t>& hizBuffer = rd->data->getHiZ();
        if (depthData)
        {
            const float bias = 3.9623753e+28f; // 1.0f / floatCompressionBias
            depthData->resize(_blockNumX * _blockNumY * 64);
            for (uint32_t y = 0; y < _blockNumY; ++y)
            {
                for (uint32_t x = 0; x < _blockNumX; ++x)
                {
                    uint32_t index = y * _blockNumX + x;
                    if (hizBuffer[index] == 1)
                    {
                        for (uint32_t subY = 0; subY < 8; ++subY)
                        {
                            std::vector<float>::iterator it = depthData->begin() + ((8 * _blockNumX) * (8 * y + subY) + 8 * x);
                            for (uint32_t subX = 0; subX < 8; ++subX, ++it) *it = -1.0f;
                        }
                        continue;
                    }

                    const __m128i* source = &depthBuffer[8 * index];
                    for (uint32_t subY = 0; subY < 8; ++subY)
                    {
                        __m128i depthI = _mm_load_si128(source++);
                        __m256i depthI256 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(depthI), 12);
                        __m256 depth = _mm256_mul_ps(_mm256_castsi256_ps(depthI256), _mm256_set1_ps(bias));
                        __m256 linDepth = _mm256_div_ps(_mm256_set1_ps(2 * 0.25f),
                                                        _mm256_sub_ps(_mm256_set1_ps(0.25f + 1000.0f),
                                                                      _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(1.0f), depth),
                                                                                    _mm256_set1_ps(1000.0f - 0.25f))));
                        float linDepthA[16]; _mm256_storeu_ps(linDepthA, linDepth);

                        std::vector<float>::iterator it = depthData->begin() + ((8 * _blockNumX) * (8 * y + subY) + 8 * x);
                        for (uint32_t subX = 0; subX < 8; ++subX, ++it) *it = linDepthA[subX];
                    }
                }
            }
        }
        if (hizData) hizData->assign(hizBuffer.begin(), hizBuffer.end());

#   if false
        std::vector<char> rawData(_blockNumX * _blockNumY * 256);
        rd->data->readBackDepth(&(*rawData.begin()));

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(_blockNumX * 8, _blockNumY * 8, 1, GL_RGBA, GL_RGBA,
                        GL_UNSIGNED_BYTE, (unsigned char*)&rawData[0], osg::Image::NO_DELETE);
        osgDB::writeImageFile(*image, "test_occlusion_depth.png");
#   endif
    }

    float UserRasterizer::queryVisibility(UserOccluder* occluder, int* numVisible)
    {
        if (!occluder || (occluder && occluder->getBatches().empty())) return 0.0f;
        std::set<osg::ref_ptr<BatchOccluder>>& batches = occluder->getBatches();
        size_t occCount = 0, count = 0, maxCount = 0;

        UserRasterizerData* rd = (UserRasterizerData*)_privateData;
        for (std::set<osg::ref_ptr<BatchOccluder>>::iterator it2 = batches.begin();
             it2 != batches.end(); ++it2)
        {
            BatchOccluder* bo = (*it2).get(); bool needsClipping = false;
            size_t numVertices = bo->getNumVertices();

            BatchOccluderData* od = (BatchOccluderData*)bo->getOccluder();
            if (rd->data->queryVisibility(od->data->m_boundsMin, od->data->m_boundsMax, needsClipping))
            { occCount++; count += numVertices; } maxCount += numVertices;
        }
        if (numVisible) *numVisible = occCount;
        return (float)count / (float)maxCount;
    }

    float UserRasterizer::queryOccludedTriangles(UserOccluder* occluder, std::vector<TriangleReference>* occludedTriangles,
                                                const UserRasterizer::OccludedTriangleCallback& callback)
    {
        if (occludedTriangles) occludedTriangles->clear();
        if (!occluder || occluder->getQuads().empty()) return 0.0f;

        const unsigned int invalidTriangle = 0xFFFFFFFF;
        const std::vector<UserOccluder::QuadInfo>& quads = occluder->getQuads();
        const std::vector<TriangleReference>& allTriangles = occluder->getTriangleReferences();
        UserRasterizerData* rd = (UserRasterizerData*)_privateData;
        size_t totalCount = 0, occludedCount = 0;

        for (size_t i = 0; i < quads.size(); ++i)
        {
            const UserOccluder::QuadInfo& quad = quads[i];
            totalCount += (quad.triangles[1] != invalidTriangle) ? 2 : 1;

            bool needsClipping = false;
            if (!rd->data->queryOccluded(convertFromVec3(quad.bounds._min),
                                         convertFromVec3(quad.bounds._max), needsClipping))
                continue;

            for (int k = 0; k < 2; ++k)
            {
                unsigned int triangleId = quad.triangles[k];
                if (triangleId == invalidTriangle) continue;
                occludedCount++;
                if (triangleId >= allTriangles.size()) continue;

                const TriangleReference& ref = allTriangles[triangleId];
                if (occludedTriangles) occludedTriangles->push_back(ref);
                if (callback) callback(ref);
            }
        }
        return (totalCount > 0) ? (float)occludedCount / (float)totalCount : 0.0f;
    }

    float OcclusionAnalyzer::findInvisibleTriangles(osg::Node& node, const osg::Matrix& view, const osg::Matrix& proj,
                                                    std::vector<TriangleReference>& invisibleTriangles,
                                                    const UserRasterizer::OccludedTriangleCallback& callback,
                                                    unsigned int width, unsigned int height)
    {
        invisibleTriangles.clear();
        width = std::max(width & (~7u), 8u); height = std::max(height & (~7u), 8u);

        osg::ref_ptr<UserOccluder> occluder = new UserOccluder(node);
        if (!occluder->hasTriangleReferences()) return 0.0f;

        // The model itself is used as the occluder, so it can hide its own triangles
        osg::ref_ptr<UserRasterizer> rasterizer = new UserRasterizer(width, height);
        return findInvisibleTriangles(*occluder, *rasterizer, view, proj, invisibleTriangles, callback);
    }

    float OcclusionAnalyzer::findInvisibleTriangles(UserOccluder& occluder, UserRasterizer& rasterizer,
                                                    const osg::Matrix& view, const osg::Matrix& proj,
                                                    std::vector<TriangleReference>& invisibleTriangles,
                                                    const UserRasterizer::OccludedTriangleCallback& callback)
    {
        invisibleTriangles.clear();
        rasterizer.addOccluder(&occluder);
        rasterizer.setModelViewProjection(view, proj);

        osg::Vec3 cameraPos = osg::Vec3() * osg::Matrix::inverse(view);
        rasterizer.render(cameraPos, NULL, NULL);
        return rasterizer.queryOccludedTriangles(&occluder, &invisibleTriangles, callback);
    }
#else
    UserOccluder::UserOccluder(const std::string& name, const std::vector<osg::Vec3> vertices,
                               const std::vector<unsigned int>& indices) : _name(name) {}
    UserOccluder::UserOccluder(osg::Geometry& geom) {}
    UserOccluder::UserOccluder(osg::Node& geom) {}
    void UserOccluder::set(const std::vector<osg::Vec3>, const std::vector<unsigned int>&,
                           const std::vector<TriangleReference>*) {}

    BatchOccluder::BatchOccluder(UserOccluder* u, const std::vector<osg::Vec3> vertices,
                                 const osg::BoundingBoxf& refBound) : _privateData(NULL), _owner(u) {}
    BatchOccluder::BatchOccluder(UserOccluder* u, void* verticesInternal,
                                 const osg::BoundingBoxf& refBound) : _privateData(NULL), _owner(u) {}
    BatchOccluder::~BatchOccluder() {}
    osg::BoundingBoxf BatchOccluder::getBound() const { osg::BoundingBoxf bb; return bb; }
    osg::Vec3 BatchOccluder::getCenter() const { osg::Vec3 vv; return vv; }

    UserRasterizer::UserRasterizer(unsigned int, unsigned int) {}
    UserRasterizer::~UserRasterizer() {}
    float UserRasterizer::queryVisibility(UserOccluder*, int*) { return 0.0f; }
    float UserRasterizer::queryOccludedTriangles(UserOccluder*, std::vector<TriangleReference>*,
                                                 const OccludedTriangleCallback&) { return 0.0f; }
    void UserRasterizer::setModelViewProjection(const osg::Matrix&, const osg::Matrix&) {}
    void UserRasterizer::render(const osg::Vec3&, std::vector<float>*, std::vector<unsigned short>*)
    { OSG_WARN << "[UserRasterizer] No AVX support, Rasterizer is not implemented" << std::endl; }
    float OcclusionAnalyzer::findInvisibleTriangles(osg::Node&, const osg::Matrix&, const osg::Matrix&,
                                                    std::vector<TriangleReference>&,
                                                    const UserRasterizer::OccludedTriangleCallback&,
                                                    unsigned int, unsigned int)
    {
        OSG_WARN << "[OcclusionAnalyzer] No AVX support, occlusion analysis is not implemented" << std::endl;
        return 0.0f;
    }

    float OcclusionAnalyzer::findInvisibleTriangles(UserOccluder&, UserRasterizer&, const osg::Matrix&,
                                                    const osg::Matrix&, std::vector<TriangleReference>&,
                                                    const UserRasterizer::OccludedTriangleCallback&)
    { return 0.0f; }
#endif

    void UserRasterizer::removeOccluder(UserOccluder* o)
    {
        std::set<osg::ref_ptr<UserOccluder>>::iterator it = _occluders.find(o);
        if (it != _occluders.end()) _occluders.erase(it);
    }
}
