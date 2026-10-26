#ifndef MANA_PP_RASTERIZER_HPP
#define MANA_PP_RASTERIZER_HPP

#include <osg/Texture2D>
#include <osg/Geometry>
#include <memory>
#include <set>
#include <functional>

//#define WITH_SIMD_INPUT 1
#ifdef WITH_SIMD_INPUT
#   include <smmintrin.h>
#endif

namespace osgVerse
{
    class UserOccluder;
    class BatchOccluder : public osg::Referenced
    {
    public:
        BatchOccluder(UserOccluder* u, const std::vector<osg::Vec3> vertices,
                      const osg::BoundingBoxf& refBound);
        BatchOccluder(UserOccluder* u, void* verticesInternal, const osg::BoundingBoxf& refBound);
        void* getOccluder() const { return _privateData; }
        UserOccluder* getOwner() { return _owner; }

        osg::BoundingBoxf getBound() const;
        osg::Vec3 getCenter() const;
        unsigned int getNumVertices() const { return _numVertices; }

    protected:
        virtual ~BatchOccluder();
        void* _privateData;
        UserOccluder* _owner;
        unsigned int _numVertices;
    };

    /** Reference to a triangle inside a source geometry */
    struct TriangleReference
    {
        osg::ref_ptr<osg::Geometry> geometry;  // source drawable
        unsigned int primitiveSet;             // index of the PrimitiveSet in the geometry
        unsigned int primitiveIndex;           // triangle index inside the PrimitiveSet,
                                               // following the triangulation of osg::TriangleIndexFunctor
                                               // (equal to the triangle index for GL_TRIANGLES)

        TriangleReference(osg::Geometry* g = NULL, unsigned int ps = 0, unsigned int pi = 0)
        :   geometry(g), primitiveSet(ps), primitiveIndex(pi) {}

        bool valid() const { return geometry.valid(); }
    };

    class UserOccluder : public osg::Referenced
    {
    public:
        UserOccluder(const std::string& name, const std::vector<osg::Vec3> vertices,
                     const std::vector<unsigned int>& indices);
#ifdef WITH_SIMD_INPUT
        UserOccluder(const std::string& name, const std::vector<__m128> vertices,
                     const std::vector<unsigned int>& indices);
#endif
        UserOccluder(osg::Geometry& geom);
        UserOccluder(osg::Node& node);

        std::set<osg::ref_ptr<BatchOccluder>>& getBatches() { return _batches; }
        const std::string& getName() const { return _name; }

        /** Source triangle references, only available when the occluder is created from a
            Geometry or a Node (used to map occluded quads back to model triangles) */
        bool hasTriangleReferences() const { return !_triangles.empty(); }
        const std::vector<TriangleReference>& getTriangleReferences() const { return _triangles; }
        unsigned int getNumTriangles() const { return (unsigned int)_triangles.size(); }

        /** Quads used by occlusion queries, each of them also references its source triangles */
        struct QuadInfo
        {
            osg::BoundingBoxf bounds;    // world-space bounds of the quad
            unsigned int triangles[2];   // source triangle ids, 0xffffffff if not available
        };
        const std::vector<QuadInfo>& getQuads() const { return _quads; }
        unsigned int getNumQuads() const { return (unsigned int)_quads.size(); }

    protected:
        virtual ~UserOccluder() {}
        void set(const std::vector<osg::Vec3> v, const std::vector<unsigned int>& i,
                 const std::vector<TriangleReference>* triRefs = NULL);

        std::set<osg::ref_ptr<BatchOccluder>> _batches;
        std::vector<TriangleReference> _triangles;
        std::vector<QuadInfo> _quads;
        std::string _name;
    };

    class UserRasterizer : public osg::Referenced
    {
    public:
        UserRasterizer(unsigned int width, unsigned int height);
        void setModelViewProjection(const osg::Matrix& view, const osg::Matrix& proj);
        void render(const osg::Vec3& cameraPos, std::vector<float>* depthData,
                    std::vector<unsigned short>* hizData = NULL);
        float queryVisibility(UserOccluder* occluder, int* numVisible = NULL);

        typedef std::function<void(const TriangleReference&)> OccludedTriangleCallback;

        /** Query triangles which are completely hidden by the rasterized depth buffer.
            Only triangles inside the frustum and provably occluded are reported (conservative),
            so the result is safe to use for deleting invisible (e.g. interior) triangles.
            @return occluded triangle ratio in [0, 1] */
        float queryOccludedTriangles(UserOccluder* occluder, std::vector<TriangleReference>* occludedTriangles = NULL,
                                     const OccludedTriangleCallback& callback = OccludedTriangleCallback());

        void addOccluder(UserOccluder* o) { _occluders.insert(o); }
        void removeOccluder(UserOccluder* o);
        void removeAllOccluders() { _occluders.clear(); }

        std::set<osg::ref_ptr<UserOccluder>>& getOccluders() { return _occluders; }
        const std::set<osg::ref_ptr<UserOccluder>>& getOccluders() const { return _occluders; }
        unsigned int getNumOccluders() const { return _occluders.size(); }

    protected:
        virtual ~UserRasterizer();
        std::set<osg::ref_ptr<UserOccluder>> _occluders;
        unsigned int _blockNumX, _blockNumY;
        void* _privateData;
    };

    /** Helper for analyzing which triangles of a model are invisible from a camera */
    class OcclusionAnalyzer : public osg::Referenced
    {
    public:
        /** Rasterize the node itself (so the model can hide its own triangles, e.g. interior
            faces of a closed mesh) and collect triangles fully occluded inside the frustum.
            Note: width and height should be multiples of 8.
            @return invisible triangle ratio in [0, 1] */
        static float findInvisibleTriangles(osg::Node& node, const osg::Matrix& view, const osg::Matrix& proj,
                                            std::vector<TriangleReference>& invisibleTriangles,
                                            const UserRasterizer::OccludedTriangleCallback& callback = UserRasterizer::OccludedTriangleCallback(),
                                            unsigned int width = 1024, unsigned int height = 1024);

        /** Same as above but reusing an already created occluder and rasterizer; this is much
            faster when the same model is analyzed from many camera views (e.g. orbit views) */
        static float findInvisibleTriangles(UserOccluder& occluder, UserRasterizer& rasterizer,
                                            const osg::Matrix& view, const osg::Matrix& proj,
                                            std::vector<TriangleReference>& invisibleTriangles,
                                            const UserRasterizer::OccludedTriangleCallback& callback = UserRasterizer::OccludedTriangleCallback());
    };
}

#endif
