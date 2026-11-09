#ifndef MANA_MODELING_GAUSSIANGEOMETRY
#define MANA_MODELING_GAUSSIANGEOMETRY

#include <osg/Version>
#if OSG_VERSION_GREATER_THAN(3, 3, 3)
#   include <osg/VertexAttribDivisor>
#endif
#if OSG_VERSION_GREATER_THAN(3, 1, 9)
#include <osg/TextureBuffer>
#endif
#include <osg/Texture2D>
#include <osg/Geometry>
#include <osg/BoundingBox>
#include <set>
#include <cstdint>
#include <vector>

namespace osgVerse
{

/** Gaussian geometry:
   - Render with mesh instances and SSBO:
     - (Method = INSTANCING, for GL 4.3 or later)
     - Original shape vertices (vec3): getVertexArray()
     - Position.xyz + Alpha: SSBO-0 (4 floats)
     - Scale.xyz + Color.r: SSBO-1 (4 halfs)
     - Quat.xyz + Color.g: SSBO-2 (4 halfs)
     - Quat.w + ReservedX2 + Color.b: SSBO-3 (4 halfs)
     - Spherical harmonics coefficients: SSBO-4 (optional, 15 * 4 halfs, alpha ignored)
     - Indices (uint): getVertexAttribArray(7)
     - [Color customizing] Tex-0 (4 floats)

   - Render with mesh instances and texture buffers (no SH):
     - (Method = INSTANCING_TEXTURE, for GL 3 or later and GLES 3.2)
     - (Method = INSTANCING_TEX2D, for GL 2 / GLES2 / GLES3)
     - Original shape vertices (vec3): getVertexArray()
     - Position.xyz + Alpha: Tex-0 (4 floats)
     - Scale.xyz + Color.r: Tex-1 (4 floats)
     - Quat.xyz + Color.g: Tex-2 (4 floats)
     - Quat.w + ReservedX2 + Color.b: Tex-3 (4 floats)
     - NO spherical harmonics coefficients!
     - Indices: getVertexAttribArray(7)
     - [Color customizing] Tex-4 (4 floats)

   - Render with geometry shader and vertex attributes:
     - (Method = GEOMETRY_SHADER, for GL 3 or later and GLES 3.2)
     - Position (vec3): getVertexArray()
     - Scale + Rotation: getVertexAttribArray(1,2)
     - Alpha (float): getVertexAttribArray(1).a()
     - Spherical harmonics coefficients
       - R-channel (dc + 15 rests): getVertexAttribArray(4,7,10,13)
       - G-channel (dc + 15 rests): getVertexAttribArray(5,8,11,14)
       - B-channel (dc + 15 rests): getVertexAttribArray(6,9,12,15)
     - Indices: getPrimitiveSet(0)
     - [Color customizing]: getVertexAttribArray(3)
*/
class GaussianGeometry : public osg::Geometry
{
public:
    enum RenderMethod { INSTANCING, INSTANCING_TEXTURE, INSTANCING_TEX2D, GEOMETRY_SHADER };
    GaussianGeometry(RenderMethod m = INSTANCING);
    GaussianGeometry(const GaussianGeometry& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
    META_Object(osgVerse, GaussianGeometry)

#if OSG_MIN_VERSION_REQUIRED(3, 3, 2)
    virtual osg::BoundingSphere computeBound() const;
    virtual osg::BoundingBox computeBoundingBox() const;
#else
    virtual osg::BoundingBox computeBound() const;
#endif
    virtual void compileGLObjects(osg::RenderInfo& renderInfo) const;

    std::pair<osg::VectorGLuint*, osg::BufferData*> getIndexArrayAndBuffer();
    static osg::Program* createProgram(osg::Shader* vs, osg::Shader* gs, osg::Shader* fs, RenderMethod m = INSTANCING);
    static osg::NodeCallback* createUniformCallback();  // for non-pipeline rendering uses

    void setColorParameters(const std::vector<osg::Vec4>& param);
    osg::Vec4* getColorParameters(bool dirty);

    bool finalize(int vOffset = 0, int vCount = 0);  // only run this after setting all attributes
    RenderMethod getRenderMethod() const { return _method; }
    void setRenderMethod(RenderMethod m) { _method = m; }
    int getNumSplats() const { return _numSplats; }
    void setNumSplats(int n) { _numSplats = n; }

    void setShDegrees(int d) { _degrees = d; checkShaderFlag(); }
    int getShDegrees() const { return _degrees; }

    void setPosition(osg::Vec3Array* v);
    void setScaleAndRotation(osg::Vec3Array* v, osg::Vec4Array* q, osg::FloatArray* a);
    void setShRed(int i, osg::Vec4Array* v);
    void setShGreen(int i, osg::Vec4Array* v);
    void setShBlue(int i, osg::Vec4Array* v);

    osg::Vec3* getPosition3();
    osg::Vec4* getPosition4();
    osg::ref_ptr<osg::Vec3Array> getScale();
    osg::ref_ptr<osg::Vec4Array> getRotation();
    osg::ref_ptr<osg::FloatArray> getAlpha();
    osg::ref_ptr<osg::Vec4Array> getShRed(int index);
    osg::ref_ptr<osg::Vec4Array> getShGreen(int index);
    osg::ref_ptr<osg::Vec4Array> getShBlue(int index);

    osg::FloatArray* getCoreDataBuffer() { return _coreBuffer.get(); }
    osg::UShortArray* getCoreAttributeBuffer() { return _coreAttrBuffer.get(); }
    osg::UShortArray* getShcoefBuffer() { return _shcoefBuffer.get(); }

    /** Divide current geometry into parts using HDBScan clustering algorithm (returns empty if not dividable)
        DONOT call finalize() before this method if you want to divide current Gaussian geometry;
        After divided, finalize() will be called automatically.
    */
    std::vector<osg::ref_ptr<osgVerse::GaussianGeometry>> divideByCluster(int vOffset = 0, int vCount = 0);

protected:
    virtual ~GaussianGeometry() {}
    osg::BoundingBox getBounding(osg::Vec4* va) const;
    void checkShaderFlag();

    std::map<std::string, std::vector<osg::Vec4>> _preDataMap;
    std::map<std::string, std::vector<osg::Vec3>> _preDataMap2;
    osg::ref_ptr<osg::FloatArray> _coreBuffer;
    osg::ref_ptr<osg::UShortArray> _coreAttrBuffer, _shcoefBuffer;
    osg::ref_ptr<osg::Texture> _coreTex[4];
    RenderMethod _method;
    int _degrees, _numSplats;
};

/** Gaussian sorter */
class GaussianSorter : public osg::Referenced
{
public:
    GaussianSorter(int numThreads = 1) : _method(CPU_SORT), _firstFrame(true), _onDemand(true)
    { configureThreads(numThreads); }

    void cull(osg::RenderInfo& renderInfo);
    void configureThreads(int numThreads);

    enum Method { CPU_SORT, USER_SORT };
    void setMethod(Method m) { _method = m; }
    Method getMethod() const { return _method; }

    void setOnDemand(bool b) { _onDemand = b; }
    bool getOnDemand() const { return _onDemand; }

    struct UserCallback : public osg::Referenced
    {
        virtual bool sort(osg::VectorGLuint* indices, osg::Vec3* pos, size_t size,
                          const osg::Matrix& model, const osg::Matrix& view) = 0;
        virtual bool sort(osg::VectorGLuint* indices, osg::Vec4* pos, size_t size,
                          const osg::Matrix& model, const osg::Matrix& view) = 0;
    };
    void setSortCallback(UserCallback* s) { _sortCallback = s; if (s) _method = USER_SORT; }
    UserCallback* getSortCallback() { return _sortCallback.get(); }

    void addGeometry(GaussianGeometry* geom);
    void removeGeometry(GaussianGeometry* geom);
    void clear() { _geometries.clear(); _geometryMatrices.clear(); }

    unsigned int size() const { return _geometries.size(); }
    unsigned int numThreads() const { return _sortThreads.size(); }

protected:
    virtual ~GaussianSorter() { configureThreads(0); }
    virtual void cull(osg::State* state, GaussianGeometry* geom, const osg::Matrix& model, const osg::Matrix& view);

    std::set<osg::ref_ptr<GaussianGeometry>> _geometries;
    std::map<GaussianGeometry*, osg::Matrix> _geometryMatrices;
    std::vector<OpenThreads::Thread*> _sortThreads;
    osg::ref_ptr<UserCallback> _sortCallback;
    Method _method; bool _firstFrame, _onDemand;
};

/** Gaussian sorter callback for use */
class GaussianSortCallback : public osg::Camera::DrawCallback
{
public:
    GaussianSortCallback(osgVerse::GaussianSorter* s)
        : _sorter(s), _lastFrame(0), _useFrameFence(false) {}
    virtual void operator()(osg::RenderInfo& renderInfo) const;
    virtual void releaseGLObjects(osg::State* state = 0) const;

    void setUseFrameFence(bool b) { _useFrameFence = b; }
    bool getUseFrameFence() const { return _useFrameFence; }

    osgVerse::GaussianSorter* getSorter() { return _sorter.get(); }
    const osgVerse::GaussianSorter* getSorter() const { return _sorter.get(); }

protected:
    osg::ref_ptr<osgVerse::GaussianSorter> _sorter;
    osg::ref_ptr<osg::Referenced> _fence;
    mutable unsigned int _lastFrame;
    bool _useFrameFence;
};

// =============================================================================
// LCC2 .btree BVH format support
// =============================================================================
static constexpr uint32_t BTREE_NODE_SIZE = 32;

/** Single BVH node (32 bytes, no dynamic allocation) */
struct BtreeNode
{
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    uint32_t field6 = 0, field7 = 0;

    bool isLeaf() const { return ((field7 >> 16) & 0xFFFF) == 0xFFFF; }
    uint32_t triangleOffset() const { return field6; }  // leaf only
    uint16_t triangleCount() const { return (uint16_t)(field7 & 0xFFFF); }  // leaf only
    uint32_t rightChildByteOffset() const { return field6 * 4; }  // internal only
    uint32_t splitAxis() const { return field7; }  // internal only (0=X, 1=Y, 2=Z)

    osg::BoundingBoxf getBounds() const
    { return osg::BoundingBoxf(osg::Vec3f(minX, minY, minZ), osg::Vec3f(maxX, maxY, maxZ)); }
};

/** Parse a single node from raw btree data at the given byte offset */
inline BtreeNode parseBtreeNode(const uint8_t* data, uint32_t byteOffset)
{
    BtreeNode node;
    const float* fp = (const float*)(data + byteOffset);
    const uint32_t* u32p = (const uint32_t*)(data + byteOffset);
    node.minX = fp[0]; node.minY = fp[1]; node.minZ = fp[2];
    node.maxX = fp[3]; node.maxY = fp[4]; node.maxZ = fp[5];
    node.field6 = u32p[6]; node.field7 = u32p[7];
    return node;
}

/** BVH btree data attached as user data to LCC2 gaussian nodes */
class BvhUserData : public osg::Object
{
public:
    BvhUserData() : osg::Object() {}
    BvhUserData(const BvhUserData& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
    : osg::Object(copy, copyop), _rawData(copy._rawData) {}
    META_Object(osgVerse, BvhUserData);

    void setRawData(const std::vector<uint8_t>& data) { _rawData = data; }
    const std::vector<uint8_t>& getRawData() const { return _rawData; }
    bool valid() const { return !_rawData.empty() && (_rawData.size() % BTREE_NODE_SIZE == 0); }
    uint32_t getNodeCount() const { return valid() ? (uint32_t)_rawData.size() / BTREE_NODE_SIZE : 0; }
    uint32_t getDataSize() const { return (uint32_t)_rawData.size(); }

    // Access a node at a given byte offset (no bounds check in release)
    BtreeNode getNode(uint32_t byteOffset) const
    {
        if (!valid() || byteOffset + BTREE_NODE_SIZE > _rawData.size()) return BtreeNode();
        return parseBtreeNode(_rawData.data(), byteOffset);
    }

    // Convenience: get root node (always at byte offset 0)
    BtreeNode getRoot() const { return getNode(0); }

protected:
    std::vector<uint8_t> _rawData;
};

}

#endif
