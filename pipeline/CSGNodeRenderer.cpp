#include <osg/GL>
#include <osg/Notify>
#include <osg/Math>
#include <osg/Shader>
#include <osg/Viewport>
#include <osg/ColorMask>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Drawable>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Scissor>
#include <osg/StateSet>
#include <osg/Stencil>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <tuple>
#include "CSGNode.h"
#include "Pipeline.h"
#include "Utilities.h"

using namespace osgVerse;

namespace
{
    // Vertex shader of the passes computing the boolean result. It has to transform the
    // vertices in the same way as the final shading (see CSGNode::setVertexShader())
    const char* defaultVertexShaderCode = {
        "void main() {\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };

    // Vertex shader for the screen quad: the quad is given in normalized device
    // coordinates already, so that a full screen quad is drawn independently of the camera
    const char* screenQuadVertexShaderCode = {
        "void main() {\n"
        "    gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);\n"
        "}\n"
    };

    // The passes of the computation never write color values; the fragment shader is only
    // required to complete the program
    const char* fragmentShaderCode = {
        "VERSE_FS_OUT vec4 fragData;\n"
        "void main() {\n"
        "    fragData = vec4(0.0);\n"
        "    VERSE_FS_FINAL(fragData);\n"
        "}\n"
    };

    osg::Program* createPassProgram(const std::string& vertexCode)
    {
        int cxtVer = 0, glslVer = 0; guessOpenGLVersions(cxtVer, glslVer);
        osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, vertexCode);
        osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, fragmentShaderCode);
        Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
        Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(vs.get()); program->addShader(fs.get());
        return program.release();
    }

    // Drawable which clears the depth and/or stencil buffer of the current target. The
    // scissor rectangle of the pass state limits it to the region of the computation
    class ClearBuffersDrawable : public osg::Drawable
    {
    public:
        ClearBuffersDrawable(unsigned int buffers) : _buffers(buffers)
        {
            setUseDisplayList(false);
            setSupportsDisplayList(false);
            setUseVertexBufferObjects(false);
            setName("osgVerse::CSGClearBuffers");
        }

        virtual osg::Object* cloneType() const { return new ClearBuffersDrawable(_buffers); }
        virtual osg::Object* clone(const osg::CopyOp& copyop) const
        { return new ClearBuffersDrawable(_buffers); }
        virtual bool isSameKindAs(const osg::Object* obj) const
        { return dynamic_cast<const ClearBuffersDrawable*>(obj) != NULL; }
        virtual const char* libraryName() const { return "osgVerse"; }
        virtual const char* className() const { return "CSGClearBuffersDrawable"; }

        virtual void drawImplementation(osg::RenderInfo& renderInfo) const
        {
            GLfloat clearDepth = 1.0f;
            GLint clearStencil = 0;
            if (_buffers & GL_DEPTH_BUFFER_BIT)
                glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
            if (_buffers & GL_STENCIL_BUFFER_BIT)
                glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clearStencil);

            // The depth buffer is reset to the near plane (0), which is required by the
            // algorithm, cf. renderSCS() of OpenCSG. GLES only provides glClearDepthf(),
            // while desktop GL keeps glClearDepth() (double)
#if defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
            glClearDepthf(0.0f);
#else
            glClearDepth(0.0);
#endif
            glClearStencil(0);
            glClear(_buffers);
#if defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
            glClearDepthf(clearDepth);
#else
            glClearDepth((GLdouble)clearDepth);
#endif
            glClearStencil(clearStencil);
        }

        virtual osg::BoundingSphere computeBound() const { return osg::BoundingSphere(); }

    protected:
        virtual ~ClearBuffersDrawable() {}

        unsigned int _buffers;
    };

    // Axis aligned bounding box in normalized device coordinates
    struct NDCBox
    {
        NDCBox() : minx(0.0f), miny(0.0f), minz(0.0f), maxx(0.0f), maxy(0.0f), maxz(0.0f) {}
        NDCBox(float nx, float ny, float nz, float xx, float xy, float xz)
        : minx(nx), miny(ny), minz(nz), maxx(xx), maxy(xy), maxz(xz) {}

        float minx, miny, minz, maxx, maxy, maxz;
    };

    float clampToViewport(double value)
    {
        return (float)((std::max)(-1.0, (std::min)(1.0, value)));
    }

    // Bounding box of a node in normalized device coordinates. All values are clamped to
    // the viewport; if the geometry cannot be projected (parts at or behind the eye
    // plane), the whole viewport is returned. The matrix must include modelview and
    // projection, but not the window (viewport) transformation
    NDCBox computeNdcBox(const osg::Node* node, const osg::Matrix& mvp)
    {
        const NDCBox fullscreen(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        if (node == NULL) return fullscreen;

        const osg::BoundingSphere& bs = node->getBound();
        if (!bs.valid()) return fullscreen;

        const osg::Vec3 center = bs.center();
        const double radius = bs.radius();
        double minx = 1.0e30, miny = 1.0e30, minz = 1.0e30;
        double maxx = -1.0e30, maxy = -1.0e30, maxz = -1.0e30;
        for (unsigned int i = 0; i < 8; ++i)
        {
            const osg::Vec3 corner(center.x() + ((i & 1) ? radius : -radius),
                                   center.y() + ((i & 2) ? radius : -radius),
                                   center.z() + ((i & 4) ? radius : -radius));
            const osg::Vec4 projected = osg::Vec4(corner, 1.0f) * mvp;
            if (projected.w() <= 1.0e-6f) return fullscreen;  // clipped by the eye plane

            const double x = projected.x() / projected.w();
            const double y = projected.y() / projected.w();
            const double z = projected.z() / projected.w();
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            if (z < minz) minz = z;
            if (z > maxz) maxz = z;
        }

        return NDCBox(clampToViewport(minx), clampToViewport(miny), clampToViewport(minz),
                      clampToViewport(maxx), clampToViewport(maxy), clampToViewport(maxz));
    }

    NDCBox intersectNdcBoxes(const NDCBox& a, const NDCBox& b)
    {
        NDCBox result;
        result.minx = (a.minx > b.minx) ? a.minx : b.minx;
        result.miny = (a.miny > b.miny) ? a.miny : b.miny;
        result.minz = (a.minz > b.minz) ? a.minz : b.minz;
        result.maxx = (a.maxx < b.maxx) ? a.maxx : b.maxx;
        result.maxy = (a.maxy < b.maxy) ? a.maxy : b.maxy;
        result.maxz = (a.maxz < b.maxz) ? a.maxz : b.maxz;
        return result;
    }

    // Object space optimization of OpenCSG (batch.cpp): several subtracted primitives are
    // treated as a single batch if they do not overlap in screen space
    typedef std::vector<CSGPrimitive*> CSGBatch;

    bool intersectXY(const NDCBox& a, const NDCBox& b)
    {
        return (b.maxx >= a.minx) && (a.maxx >= b.minx)
            && (b.maxy >= a.miny) && (a.maxy >= b.miny);
    }

    bool containsXY(const NDCBox& a, const NDCBox& b)
    {
        return (a.minx >= b.minx) && (a.maxx <= b.maxx)
            && (a.miny >= b.miny) && (a.maxy <= b.maxy);
    }

    std::vector<CSGBatch> batchPrimitives(const std::vector<CSGPrimitive*>& primitives,
                                          const std::vector<NDCBox>& ndcBoxes)
    {
        const NDCBox fullscreen(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        typedef std::vector<std::size_t> IndexBatch;

        std::vector<IndexBatch> candidates, finished;
        for (std::size_t i = 0; i < primitives.size(); ++i)
        {
            const NDCBox& box = ndcBoxes[i];
            if (!intersectXY(box, fullscreen)) continue;  // completely outside the viewport

            // the viewport is completely part of the primitive's bounding box, so no other
            // primitive can be part of the same batch
            if (containsXY(fullscreen, box))
            { finished.push_back(IndexBatch(1, i)); continue; }

            std::size_t b = 0;
            for (; b < candidates.size(); ++b)
            {
                IndexBatch& batch = candidates[b];
                IndexBatch::const_iterator itr = batch.begin();
                for (; itr != batch.end(); ++itr)
                {
                    if (intersectXY(box, ndcBoxes[*itr])) break;
                }
                if (itr == batch.end()) { batch.push_back(i); break; }
            }
            if (b == candidates.size()) candidates.push_back(IndexBatch(1, i));
        }

        std::vector<CSGBatch> batches;
        batches.reserve(finished.size() + candidates.size());
        for (std::size_t i = 0; i < finished.size(); ++i)
        {
            batches.push_back(CSGBatch());
            for (std::size_t j = 0; j < finished[i].size(); ++j)
                batches.back().push_back(primitives[finished[i][j]]);
        }
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            batches.push_back(CSGBatch());
            for (std::size_t j = 0; j < candidates[i].size(); ++j)
                batches.back().push_back(primitives[candidates[i][j]]);
        }
        return batches;
    }

    // Permutation embedding sequence by Schoenfield (cf. OpenCSG's SchoenfieldSequencer),
    // used when the depth complexity is unknown
    struct SchoenfieldSequencer
    {
        explicit SchoenfieldSequencer(std::size_t n) : _n(n) {}

        std::size_t index(std::size_t position) const
        {
            if (_n == 1) return 0;
            if (_n == 2) return position & 1;
            if (position < _n) return position;
            else if ((position - 1) % (_n - 1) == 0) return 0;
            else return (position * (_n - 2) / (_n - 1)) % (_n - 1) + 1;
        }

        std::size_t size() const
        {
            if (_n == 1) return 1;
            if (_n == 2) return 3;
            return _n * _n - 2 * _n + 4;
        }

        std::size_t _n;
    };

    // "Bouncing" sequence, needs fewer passes if the depth complexity is known
    // (cf. OpenCSG's BouncingSequencer)
    struct BouncingSequencer
    {
        explicit BouncingSequencer(std::size_t n) : _n(n) {}

        std::size_t index(std::size_t position) const
        {
            if (_n == 1) return 0;
            std::size_t wave = position % (_n + _n - 2);
            if (wave < _n) return wave;
            else return (_n + _n - 2) - wave;
        }

        std::size_t sizeForDepthComplexity(std::size_t dc) const
        {
            return ((dc & 1) == 1) ? (dc - 1) * (_n - 1) + _n : dc * (_n - 1) + 1;
        }

        std::size_t _n;
    };

    // The visible surface of a subtracted primitive is its back face, so its depth is
    // written where the front faces have been visible before (cf. subtractPrimitives()
    // of the original implementation)
    void subtractBatch(CSGNodeRenderer& target, osgUtil::CullVisitor& cv,
                       const CSGBatch& batch, unsigned int stencilRef, bool cameraInsideModel)
    {
        if (cameraInsideModel)
        {
            // mark the front faces which are in front of the current surface, then remove
            // the mark again where the depth test fails (the depth buffer is not written)
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::GREATER, false);
            state.cullFace = GL_FRONT;
            state.setStencil(osg::Stencil::ALWAYS, (int)stencilRef, 0xff,
                             osg::Stencil::KEEP, osg::Stencil::KEEP, osg::Stencil::REPLACE);
            target.drawPrimitives(cv, batch, state);

            state.stencilFunc = osg::Stencil::EQUAL;
            state.stencilDepthPassOp = osg::Stencil::KEEP;
            state.stencilFailOp = osg::Stencil::ZERO;
            state.stencilDepthFailOp = osg::Stencil::ZERO;
            target.drawPrimitives(cv, batch, state);
        }

        // mark the front faces which are in front of the current surface
        {
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::LESS, false);
            state.cullFace = GL_BACK;
            state.setStencil(cameraInsideModel ? osg::Stencil::EQUAL : osg::Stencil::ALWAYS,
                             (int)stencilRef, 0xff, osg::Stencil::KEEP, osg::Stencil::KEEP,
                             cameraInsideModel ? osg::Stencil::KEEP : osg::Stencil::REPLACE);
            target.drawPrimitives(cv, batch, state);
        }

        // where the front faces have been visible, write the depth of the back faces
        // (i.e. the surfaces of the cut)
        {
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::GREATER, true);
            state.cullFace = GL_FRONT;
            state.setStencil(osg::Stencil::EQUAL, (int)stencilRef, 0xff,
                             osg::Stencil::ZERO, osg::Stencil::ZERO, osg::Stencil::ZERO);
            target.drawPrimitives(cv, batch, state);
        }
    }
}

// Workspace of the frame buffer renderer, one entry per render stage. The passes need
// distinct StateSet objects because OSG keeps the leaves of equal state sets in a single
// state graph node, which would destroy the order of the passes
struct CSGNodeRenderer::RenderData
{
    struct DepthKey
    {
        DepthKey(osg::Depth::Function f, bool w, bool r, double n, double x)
        : func(f), write(w), useRange(r), zNear(n), zFar(x) {}

        bool operator<(const DepthKey& rhs) const
        { return std::tie(func, write, useRange, zNear, zFar)
              < std::tie(rhs.func, rhs.write, rhs.useRange, rhs.zNear, rhs.zFar); }

        osg::Depth::Function func;
        bool write, useRange;
        double zNear, zFar;
    };

    struct StencilKey
    {
        explicit StencilKey(const CSGPassState& s)
        : func(s.stencilFunc), ref(s.stencilRef), funcMask(s.stencilFuncMask)
        , sfail(s.stencilFailOp), zfail(s.stencilDepthFailOp)
        , zpass(s.stencilDepthPassOp), writeMask(s.stencilWriteMask) {}

        bool operator<(const StencilKey& rhs) const
        { return std::tie(func, ref, funcMask, sfail, zfail, zpass, writeMask)
              < std::tie(rhs.func, rhs.ref, rhs.funcMask, rhs.sfail, rhs.zfail, rhs.zpass,
                         rhs.writeMask); }

        osg::Stencil::Function func;
        int ref;
        unsigned int funcMask;
        osg::Stencil::Operation sfail, zfail, zpass;
        unsigned int writeMask;
    };

    struct Workspace
    {
        Workspace() : useScissor(false), usedPasses(0)
        { scissorRect[0] = scissorRect[1] = scissorRect[2] = scissorRect[3] = 0; }

        std::vector<osg::ref_ptr<osg::StateSet>> passStates;
        osg::ref_ptr<osg::Scissor> scissor;
        int scissorRect[4];
        bool useScissor;
        unsigned int usedPasses;
    };

    RenderData()
    {
        colorMaskOn = new osg::ColorMask(true, true, true, true);
        colorMaskOff = new osg::ColorMask(false, false, false, false);
        cullFront = new osg::CullFace(osg::CullFace::FRONT);
        cullBack = new osg::CullFace(osg::CullFace::BACK);
    }

    osg::ref_ptr<osg::ColorMask> colorMaskOn, colorMaskOff;
    osg::ref_ptr<osg::CullFace> cullFront, cullBack;
    osg::ref_ptr<osg::Program> geometryProgram, quadProgram;
    osg::ref_ptr<osg::Geometry> quad;
    osg::ref_ptr<ClearBuffersDrawable> clearDepthStencil, clearStencil;
    std::string programShaderSource;

    std::map<DepthKey, osg::ref_ptr<osg::Depth>> depthAttributes;
    std::map<StencilKey, osg::ref_ptr<osg::Stencil>> stencilAttributes;
    std::map<osgUtil::RenderStage*, Workspace> workspaces;
};

CSGPassState::CSGPassState()
{
    colorWrite = false; depthFunc = osg::Depth::LESS; depthWrite = false;
    useDepthRange = false; depthRangeNear = 0.0; depthRangeFar = 1.0;
    stencilTest = false; stencilFunc = osg::Stencil::ALWAYS; stencilRef = 0;
    stencilFuncMask = 0xff; stencilFailOp = osg::Stencil::KEEP;
    stencilDepthFailOp = osg::Stencil::KEEP; stencilDepthPassOp = osg::Stencil::KEEP;
    stencilWriteMask = 0xff; cullFace = 0; shadingPass = false;
}

void CSGPassState::setDepth(osg::Depth::Function func, bool write)
{
    depthFunc = func; depthWrite = write;
}

void CSGPassState::setDepthRange(double zNear, double zFar)
{
    useDepthRange = true; depthRangeNear = zNear; depthRangeFar = zFar;
}

void CSGPassState::setStencil(osg::Stencil::Function func, int ref, unsigned int funcMask,
                              osg::Stencil::Operation sfail, osg::Stencil::Operation zfail,
                              osg::Stencil::Operation zpass, unsigned int writeMask)
{
    stencilTest = true; stencilFunc = func; stencilRef = ref; stencilFuncMask = funcMask;
    stencilFailOp = sfail; stencilDepthFailOp = zfail; stencilDepthPassOp = zpass;
    stencilWriteMask = writeMask;
}

CSGNodeRenderer::CSGNodeRenderer()
{
    _passCount = 0; _renderData = new RenderData;
}

CSGNodeRenderer::~CSGNodeRenderer()
{
    delete _renderData;
}

CSGNodeRenderer::RenderData& CSGNodeRenderer::getRenderData()
{
    return *_renderData;
}

osg::Depth* CSGNodeRenderer::getDepthAttribute(const CSGPassState& state)
{
    const RenderData::DepthKey key(state.depthFunc, state.depthWrite, state.useDepthRange,
                                   state.depthRangeNear, state.depthRangeFar);
    std::map<RenderData::DepthKey, osg::ref_ptr<osg::Depth>>::iterator itr =
        _renderData->depthAttributes.find(key);
    if (itr != _renderData->depthAttributes.end()) return itr->second.get();

    osg::Depth* depth = new osg::Depth(state.depthFunc,
                                       state.useDepthRange ? state.depthRangeNear : 0.0,
                                       state.useDepthRange ? state.depthRangeFar : 1.0,
                                       state.depthWrite);
    _renderData->depthAttributes[key] = depth;
    return depth;
}

osg::Stencil* CSGNodeRenderer::getStencilAttribute(const CSGPassState& state)
{
    const RenderData::StencilKey key(state);
    std::map<RenderData::StencilKey, osg::ref_ptr<osg::Stencil>>::iterator itr =
        _renderData->stencilAttributes.find(key);
    if (itr != _renderData->stencilAttributes.end()) return itr->second.get();

    osg::Stencil* stencil = new osg::Stencil;
    stencil->setFunction(state.stencilFunc, state.stencilRef, state.stencilFuncMask);
    stencil->setOperation(state.stencilFailOp, state.stencilDepthFailOp,
                          state.stencilDepthPassOp);
    stencil->setWriteMask(state.stencilWriteMask);
    _renderData->stencilAttributes[key] = stencil;
    return stencil;
}

osg::CullFace* CSGNodeRenderer::getCullFaceAttribute(int face)
{
    return (face == GL_FRONT) ? _renderData->cullFront.get() : _renderData->cullBack.get();
}

osg::Program* CSGNodeRenderer::getPassProgram(bool screenQuad)
{
    const std::string vertexCode = screenQuad ? std::string(screenQuadVertexShaderCode)
        : (_vertexShader.empty() ? std::string(defaultVertexShaderCode) : _vertexShader);

    if (screenQuad)
    {
        if (!_renderData->quadProgram.valid())
            _renderData->quadProgram = createPassProgram(vertexCode);
        return _renderData->quadProgram.get();
    }

    if (_renderData->geometryProgram.valid()
        && _renderData->programShaderSource == vertexCode)
        return _renderData->geometryProgram.get();

    _renderData->programShaderSource = vertexCode;
    _renderData->geometryProgram = createPassProgram(vertexCode);
    return _renderData->geometryProgram.get();
}

osg::Geometry* CSGNodeRenderer::getQuadGeometry()
{
    if (_renderData->quad.valid()) return _renderData->quad.get();

    osg::Vec3Array* vertices = new osg::Vec3Array(4);
    (*vertices)[0].set(-1.0f, -1.0f, 0.0f);
    (*vertices)[1].set( 1.0f, -1.0f, 0.0f);
    (*vertices)[2].set( 1.0f,  1.0f, 0.0f);
    (*vertices)[3].set(-1.0f,  1.0f, 0.0f);

    osg::Geometry* quad = new osg::Geometry;
    quad->setName("osgVerse::CSGScreenQuad");
    quad->setVertexArray(vertices);
    quad->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));
    quad->setUseDisplayList(false);
    quad->setSupportsDisplayList(false);
    quad->setUseVertexBufferObjects(true);
    _renderData->quad = quad;
    return quad;
}

osg::Drawable* CSGNodeRenderer::getClearDrawable(unsigned int glBuffers)
{
    if (glBuffers & GL_DEPTH_BUFFER_BIT)
    {
        if (!_renderData->clearDepthStencil.valid())
            _renderData->clearDepthStencil =
                new ClearBuffersDrawable(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        return _renderData->clearDepthStencil.get();
    }

    if (!_renderData->clearStencil.valid())
        _renderData->clearStencil = new ClearBuffersDrawable(GL_STENCIL_BUFFER_BIT);
    return _renderData->clearStencil.get();
}

osg::StateSet* CSGNodeRenderer::preparePassState(osgUtil::CullVisitor& cv,
                                                 const CSGPassState& state, bool screenQuad)
{
    RenderData& data = getRenderData();
    if (data.workspaces.size() > 8)
    {
        // don't keep the pass state of render stages which are not used any more
        // (each camera/view creates its own render stage)
        data.workspaces.clear();
    }

    RenderData::Workspace& ws = data.workspaces[cv.getCurrentRenderStage()];
    const unsigned int index = ws.usedPasses++;
    if (index >= ws.passStates.size()) ws.passStates.resize(index + 1);

    osg::ref_ptr<osg::StateSet>& entry = ws.passStates[index];
    if (!entry.valid())
    {
        entry = new osg::StateSet;
        entry->setThreadSafeRefUnref(true);
        entry->setName("osgVerse::CSGPass");
    }

    osg::StateSet* stateset = entry.get();
    stateset->clear();

    // The passes must be drawn in exactly the order in which they are emitted: the cull
    // visitor would move the geometry of a nested render bin (e.g. the transparent
    // "DepthSortedBin" of a loaded model) into a separate bin. OVERRIDE_RENDERBIN_DETAILS
    // without a bin name makes the cull visitor ignore the render bin details of the
    // primitives, while the pass itself stays in the render bin of the CSG node
    stateset->setRenderBinMode(osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
    const osg::StateAttribute::GLModeValue overrideValue =
        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE;

    // the boolean result is computed in the depth and stencil buffers only, so no pass
    // writes color values (except the final shading pass)
    stateset->setAttribute(state.colorWrite ? _renderData->colorMaskOn.get()
                                            : _renderData->colorMaskOff.get(), overrideValue);

    stateset->setAttribute(getDepthAttribute(state), overrideValue);
    stateset->setMode(GL_DEPTH_TEST, overrideValue);

    if (state.cullFace != 0)
    {
        stateset->setAttribute(getCullFaceAttribute(state.cullFace), overrideValue);
        stateset->setMode(GL_CULL_FACE, overrideValue);
    }
    else
    {
        stateset->setMode(GL_CULL_FACE,
                          osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    }

    stateset->setAttribute(getStencilAttribute(state), overrideValue);
    stateset->setMode(GL_STENCIL_TEST, state.stencilTest ? overrideValue
                        : (osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE));

    // the region of the computation
    if (ws.useScissor && ws.scissor.valid())
    {
        stateset->setAttribute(ws.scissor.get(), overrideValue);
        stateset->setMode(GL_SCISSOR_TEST, overrideValue);
    }
    else
    {
        stateset->setMode(GL_SCISSOR_TEST,
                          osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    }

    // the passes computing the boolean result use their own program; the shading pass
    // keeps the program (and the material) of the geometry
    if (!state.shadingPass)
        stateset->setAttribute(getPassProgram(screenQuad), overrideValue);

    ++_passCount;
    return stateset;
}

void CSGNodeRenderer::beginComputation(osgUtil::CullVisitor& cv,
                                       const std::vector<CSGPrimitive*>& primitives)
{
    RenderData& data = getRenderData();
    if (data.workspaces.size() > 8)
    {
        // don't keep the pass state of render stages which are not used any more
        data.workspaces.clear();
    }

    RenderData::Workspace& ws = data.workspaces[cv.getCurrentRenderStage()];
    ws.usedPasses = 0; _passCount = 0;

    // restrict the computation to the screen space bounding box of the intersected
    // primitives: the result cannot be outside it
    osg::Viewport* viewport = cv.getViewport();
    if (viewport == NULL || viewport->width() <= 0.0 || viewport->height() <= 0.0)
    {
        OSG_INFO << "[CSGNodeRenderer] No viewport, the computation cannot be restricted "
                 << "to a scissor rectangle." << std::endl;
        ws.useScissor = false;
        return;
    }

    const osg::Matrix mvp = (*cv.getModelViewMatrix()) * (*cv.getProjectionMatrix());
    NDCBox box(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    for (std::size_t i = 0; i < primitives.size(); ++i)
        box = intersectNdcBoxes(box, computeNdcBox(primitives[i]->getGeometry(), mvp));

    const double vx = viewport->x(), vy = viewport->y();
    const double vw = viewport->width(), vh = viewport->height();
    int x0 = (int)std::floor(vx + (box.minx * 0.5 + 0.5) * vw);
    int y0 = (int)std::floor(vy + (box.miny * 0.5 + 0.5) * vh);
    int x1 = (int)std::ceil (vx + (box.maxx * 0.5 + 0.5) * vw);
    int y1 = (int)std::ceil (vy + (box.maxy * 0.5 + 0.5) * vh);

    const int minX = (int)vx, minY = (int)vy;
    const int maxX = (int)(vx + vw), maxY = (int)(vy + vh);
    x0 = osg::maximum(x0, minX); y0 = osg::maximum(y0, minY);
    x1 = osg::minimum(x1, maxX); y1 = osg::minimum(y1, maxY);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;

    ws.useScissor = true;
    if (!ws.scissor.valid() || ws.scissorRect[0] != x0 || ws.scissorRect[1] != y0
        || ws.scissorRect[2] != x1 - x0 || ws.scissorRect[3] != y1 - y0)
    {
        // a new object is created whenever the region changes: OSG only applies a state
        // attribute again if it is not the one applied last
        ws.scissor = new osg::Scissor(x0, y0, x1 - x0, y1 - y0);
        ws.scissorRect[0] = x0; ws.scissorRect[1] = y0;
        ws.scissorRect[2] = x1 - x0; ws.scissorRect[3] = y1 - y0;
    }
}

void CSGNodeRenderer::endComputation(osgUtil::CullVisitor& cv)
{}

void CSGNodeRenderer::clearBuffers(osgUtil::CullVisitor& cv, unsigned int buffers,
                                   const CSGPassState& state)
{
    unsigned int glBuffers = 0;
    if (buffers & DEPTH_BUFFER) glBuffers |= GL_DEPTH_BUFFER_BIT;
    if (buffers & STENCIL_BUFFER) glBuffers |= GL_STENCIL_BUFFER_BIT;
    if (glBuffers == 0) return;

    osg::StateSet* stateset = preparePassState(cv, state, false);
    cv.pushStateSet(stateset);
    cv.addDrawableAndDepth(getClearDrawable(glBuffers), cv.getModelViewMatrix(), 0.0f);
    cv.popStateSet();
}

void CSGNodeRenderer::drawPrimitives(osgUtil::CullVisitor& cv,
                                     const std::vector<CSGPrimitive*>& primitives,
                                     const CSGPassState& state)
{
    if (primitives.empty()) return;

    osg::StateSet* stateset = preparePassState(cv, state, false);
    cv.pushStateSet(stateset);

    // the CSG node has been tested against the view frustum as a whole; the individual
    // primitives must not be culled (an invisible primitive may still be needed)
    cv.pushCullingSet();
    cv.getCurrentCullingSet().setCullingMask(osg::CullingSet::NO_CULLING);
    for (std::size_t i = 0; i < primitives.size(); ++i)
    {
        osg::Node* geometry = primitives[i] ? primitives[i]->getGeometry() : NULL;
        if (geometry != NULL) geometry->accept(cv);
    }
    cv.popCullingSet();
    cv.popStateSet();
}

void CSGNodeRenderer::drawScreenQuad(osgUtil::CullVisitor& cv, const CSGPassState& state)
{
    osg::StateSet* stateset = preparePassState(cv, state, true);
    cv.pushStateSet(stateset);
    cv.addDrawableAndDepth(getQuadGeometry(), cv.getModelViewMatrix(), 0.0f);
    cv.popStateSet();
}

void CSGNodeRenderer::render(osgUtil::CullVisitor& cv, CSGNode& node)
{
    std::vector<CSGPrimitive*> intersected, subtracted;
    intersected.reserve(node.getNumPrimitives());
    subtracted.reserve(node.getNumPrimitives());
    for (unsigned int i = 0; i < node.getNumPrimitives(); ++i)
    {
        CSGPrimitive* primitive = node.getPrimitive(i);
        if (primitive == NULL || primitive->getGeometry() == NULL) continue;

        if (primitive->getOperation() == CSG_INTERSECTION) intersected.push_back(primitive);
        else subtracted.push_back(primitive);
    }

    if (intersected.empty())
    {
        // nothing to display, cf. OpenCSG::renderDispatch()
        OSG_INFO << "[CSGNodeRenderer] The CSG node has no primitive with operation "
                 << "'Intersection', nothing is drawn." << std::endl;
        return;
    }

    beginComputation(cv, intersected);

    // Reset the depth buffer of the region to the near plane: the "furthest front face"
    // pass below writes the depth of the front faces with GL_GREATER, which requires the
    // depth values to start at the near plane. Fragments which are not covered by any
    // front face keep that value, i.e. they are marked as "not part of the result"
    CSGPassState clearState;
    clearState.colorWrite = false;
    clearState.setDepth(osg::Depth::ALWAYS, true);
    clearState.cullFace = 0;
    clearBuffers(cv, DEPTH_BUFFER | STENCIL_BUFFER, clearState);

    // ------------------------------------------------------------------
    // visibility of the intersected primitives
    // ------------------------------------------------------------------
    if (intersected.size() == 1)
    {
        // optimization of the original implementation: the front faces of a single
        // convex primitive are the result already
        CSGPassState state;
        state.colorWrite = false;
        state.setDepth(osg::Depth::GREATER, true);
        state.cullFace = GL_BACK;
        drawPrimitives(cv, intersected, state);
    }
    else
    {
        // draw the furthest front face of the intersected primitives (the intersection of
        // convex primitives is convex, so the furthest front face is the surface of the
        // intersection which faces the camera)
        {
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::GREATER, true);
            state.cullFace = GL_BACK;
            drawPrimitives(cv, intersected, state);
        }

        // count the back faces behind that front face: the fragment is part of the
        // intersection if and only if the number of those back faces is the number of
        // the intersected primitives
        {
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::GREATER, false);
            state.cullFace = GL_FRONT;
            state.setStencil(osg::Stencil::ALWAYS, 0, 0xff, osg::Stencil::KEEP,
                             osg::Stencil::KEEP, osg::Stencil::INCR);
            drawPrimitives(cv, intersected, state);
        }

        // reset the depth value of all fragments which are not part of the intersection
        // (the screen quad is drawn with the depth range [0,0])
        {
            CSGPassState state;
            state.colorWrite = false;
            state.setDepth(osg::Depth::ALWAYS, true);
            state.setDepthRange(0.0, 0.0);
            state.cullFace = 0;
            state.setStencil(osg::Stencil::NOTEQUAL, (int)intersected.size(), 0xff,
                             osg::Stencil::ZERO, osg::Stencil::ZERO, osg::Stencil::ZERO);
            drawScreenQuad(cv, state);
        }
    }

    // ------------------------------------------------------------------
    // subtraction
    // ------------------------------------------------------------------
    if (!subtracted.empty())
    {
        // the batches consist of primitives which do not overlap in screen space, so their
        // stencil operations can be done in a single pass
        const osg::Matrix mvp = (*cv.getModelViewMatrix()) * (*cv.getProjectionMatrix());
        std::vector<NDCBox> boxes(subtracted.size());
        for (std::size_t i = 0; i < subtracted.size(); ++i)
            boxes[i] = computeNdcBox(subtracted[i]->getGeometry(), mvp);

        const std::vector<CSGBatch> batches = batchPrimitives(subtracted, boxes);
        SchoenfieldSequencer schoenfield(batches.size());
        BouncingSequencer bouncing(batches.size());
        std::size_t numIterations = schoenfield.size();
        if (node.getSequencingMode() == CSG_BOUNCING_SEQUENCING)
        {
            // without a known depth complexity, all batches are assumed to overlap in depth
            const std::size_t dc = node.getDepthComplexity()
                ? (std::min)((std::size_t)node.getDepthComplexity(), batches.size())
                : batches.size();
            numIterations = bouncing.sizeForDepthComplexity(dc);
        }

        // each iteration needs a distinct stencil reference value
        const unsigned int stencilMax = 1u << node.getStencilBits();
        unsigned int stencilRef = 0;
        for (std::size_t i = 0; i < numIterations; ++i)
        {
            const std::size_t batchIndex = (node.getSequencingMode() == CSG_BOUNCING_SEQUENCING)
                                         ? bouncing.index(i) : schoenfield.index(i);
            const CSGBatch& batch = batches[batchIndex];
            if (batch.empty()) continue;

            ++stencilRef;
            if (stencilRef >= stencilMax)
            {
                // the stencil buffer has to be recycled
                clearBuffers(cv, STENCIL_BUFFER, clearState);
                stencilRef = 1;
            }
            subtractBatch(*this, cv, batch, stencilRef, node.getCameraInsideModel());
        }
    }

    // ------------------------------------------------------------------
    // Fragments where the surface of a subtracted primitive ended up behind the surface of
    // the intersected primitives do not show the result but the back faces of the
    // intersected primitives, which are not known any more; mark them as "not part of the
    // result" as well (cf. renderIntersectedBack() of the original implementation)
    // ------------------------------------------------------------------
    clearBuffers(cv, STENCIL_BUFFER, clearState);
    {
        CSGPassState state;
        state.colorWrite = false;
        state.setDepth(osg::Depth::LESS, false);
        state.cullFace = GL_FRONT;
        state.setStencil(osg::Stencil::ALWAYS, 1, 0xff, osg::Stencil::KEEP,
                         osg::Stencil::KEEP, osg::Stencil::REPLACE);
        drawPrimitives(cv, intersected, state);
    }
    {
        CSGPassState state;
        state.colorWrite = false;
        state.setDepth(osg::Depth::ALWAYS, true);
        state.setDepthRange(0.0, 0.0);
        state.cullFace = 0;
        state.setStencil(osg::Stencil::EQUAL, 1, 0xff, osg::Stencil::KEEP,
                         osg::Stencil::KEEP, osg::Stencil::ZERO);
        drawScreenQuad(cv, state);
    }

    // ------------------------------------------------------------------
    // shade the result with the primitives' own materials
    // ------------------------------------------------------------------
    if (node.getShadingPassEnabled())
    {
        // the visible surface of an intersected primitive is one of its front faces ...
        CSGPassState state;
        state.colorWrite = true;
        state.shadingPass = true;
        state.setDepth(osg::Depth::EQUAL, true);
        state.cullFace = GL_BACK;
        state.stencilTest = false;
        drawPrimitives(cv, intersected, state);

        // ... while the surface of the cut is a back face of a subtracted primitive
        if (!subtracted.empty())
        {
            state.cullFace = GL_FRONT;
            drawPrimitives(cv, subtracted, state);
        }
    }

    endComputation(cv);
}
