#include "VirtualShadowPageRenderer.h"
#include "../Global.h"
#include "../Pipeline.h"
#include "../ShaderLibrary.h"
#include "../Utilities.h"
#include <osg/CullFace>
#include <osg/PolygonOffset>
#include <osgDB/ReadFile>
#include <osgViewer/View>
#include <iostream>

#ifndef GL_DEPTH_CLAMP
#   define GL_DEPTH_CLAMP 0x864F
#endif

namespace osgVerse
{
    VirtualShadowPageRenderer::VirtualShadowPageRenderer()
    :   _casterMask(0), _concurrency(0), _nextSlot(0), _tileResolution(0),
        _atlasResolution(0), _renderOrderBase(-1000), _enabled(true)
    {}

    VirtualShadowPageRenderer::~VirtualShadowPageRenderer()
    {}

    bool VirtualShadowPageRenderer::configure(osg::GraphicsContext* gc, osg::Node* casterScene,
                                              unsigned int casterMask, int concurrency,
                                              const VirtualShadowSettings& settings,
                                              osg::Texture2D* atlas, int glVersion, int glslVersion)
    {
        if (!atlas) return false;

        _cameras.clear(); _scissors.clear();
        _nextSlot = 0; _enabled = true;
        _concurrency = (concurrency > 0) ? concurrency : 1;
        _tileResolution = settings.tileResolution;
        _atlasResolution = atlas->getTextureWidth();
        _atlasTexture = atlas; _casterMask = casterMask;
        _root = new osg::Group;

        // Depth-only caster program
        osg::ref_ptr<osg::Shader> castVS = osgDB::readShaderFile(
            osg::Shader::VERTEX, SHADER_DIR + "vsm/vsm_depth_cast.vert.glsl");
        osg::ref_ptr<osg::Shader> castFS = osgDB::readShaderFile(
            osg::Shader::FRAGMENT, SHADER_DIR + "vsm/vsm_depth_cast.frag.glsl");
        if (!castVS.valid() || !castFS.valid())
        {
            OSG_WARN << "[VirtualShadow] Failed to load the page depth cast shaders" << std::endl;
            return false;
        }
        Pipeline::createShaderDefinitions(castVS.get(), glVersion, glslVersion);
        Pipeline::createShaderDefinitions(castFS.get(), glVersion, glslVersion);
        _pageProgram = new osg::Program;
        _pageProgram->setName("VirtualShadowPageCaster");
        _pageProgram->addShader(castVS.get());
        _pageProgram->addShader(castFS.get());

        for (int slot = 0; slot < _concurrency; ++slot)
        {
            osg::Camera* camera = createPageCamera(
                slot, _tileResolution, casterMask, gc, casterScene);
            _cameras.push_back(camera);
        }
        return true;
    }

    osg::Camera* VirtualShadowPageRenderer::createPageCamera(
        int slot, int tileResolution, unsigned int casterMask,
        osg::GraphicsContext* gc, osg::Node* casterScene)
    {
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        camera->setGraphicsContext(gc);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->setRenderOrder(osg::Camera::PRE_RENDER, _renderOrderBase + slot);
        camera->setViewport(0, 0, tileResolution, tileResolution);
        camera->setClearColor(osg::Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        camera->setClearDepth(1.0);
        // Cleared and culled only while the slot actually renders a page this frame; an idle
        // slot must neither wipe its previous tile nor draw anything
        camera->setClearMask(0);
        camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setAllowEventFocus(false);
        camera->setCullMask(casterMask);
        camera->setNodeMask(~static_cast<unsigned int>(0));
        camera->setUserValue("PipelineCullMask", (unsigned int)0);
        camera->attach(osg::Camera::COLOR_BUFFER0, _atlasTexture.get());
        if (casterScene != NULL) camera->addChild(casterScene);

        int value = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE;
        osg::StateSet* ss = camera->getOrCreateStateSet();
        ss->setAttributeAndModes(_pageProgram.get(), value);
        ss->setAttributeAndModes(new osg::CullFace(osg::CullFace::FRONT), value);
        ss->setAttribute(new osg::PolygonOffset(1.1f, 4.0f), value);
        ss->setMode(GL_POLYGON_OFFSET_FILL, value);
        ss->setMode(GL_DEPTH_TEST, value);
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
        ss->setMode(GL_DEPTH_CLAMP, value);
#endif

        // The scissor confines both glClear and the caster draw to the page's atlas tile
        osg::ref_ptr<osg::Scissor> scissor = new osg::Scissor(0, 0, tileResolution, tileResolution);
        ss->setAttributeAndModes(scissor.get(), value);
        _scissors.push_back(scissor);
        return camera.release();
    }

    void VirtualShadowPageRenderer::addToView(osgViewer::View* view)
    {
        if (view == NULL) return;
        osg::Matrixd identity;
        for (size_t i = 0; i < _cameras.size(); ++i)
        {
            // Each page camera is a dedicated slave so it is rendered exactly once. Nesting it
            // in the main scene would let every pipeline stage traverse (and clear) it.
            // useBaseViewMatrix is false because each page owns an absolute light-space
            // view/projection.
            view->addSlave(_cameras[i].get(), identity, identity, false);
        }
    }

    void VirtualShadowPageRenderer::beginFrame()
    {
        _nextSlot = 0;
        for (size_t i = 0; i < _cameras.size(); ++i)
        {
            _cameras[i]->setClearMask(0);
            _cameras[i]->setUserValue("PipelineCullMask", (unsigned int)0);
        }
    }

    int VirtualShadowPageRenderer::renderPage(VirtualShadowMap* vsm,
                                              const VirtualShadowPageKey& key,
                                              const osg::Matrixd& view,
                                              const osg::Matrixd& projection,
                                              const osg::Matrix& worldToShadow)
    {
        if (!_enabled || !_root.valid() || vsm == NULL) return -1;
        if (_nextSlot >= (int)_cameras.size()) return -1;

        int tile = vsm->commitPage(key, worldToShadow);
        if (tile < 0) return -1;

        int slot = _nextSlot++;
        osg::Camera* camera = _cameras[slot].get();
        camera->setViewMatrix(view);
        camera->setProjectionMatrix(projection);

        osg::ref_ptr<VirtualShadowTilePool> pool = vsm->getTilePool();
        int tileX = pool->getTileX(tile), tileY = pool->getTileY(tile);
        camera->setViewport(tileX * _tileResolution, tileY * _tileResolution,
                            _tileResolution, _tileResolution);
        if (slot < (int)_scissors.size() && _scissors[slot].valid())
            _scissors[slot]->setScissor(tileX * _tileResolution, tileY * _tileResolution,
                                        _tileResolution, _tileResolution);

        camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        camera->setUserValue("PipelineCullMask", _casterMask);
        return tile;
    }

    void VirtualShadowPageRenderer::finalize()
    {}

    void VirtualShadowPageRenderer::setEnabled(bool b)
    {
        _enabled = b;
        if (_root.valid()) _root->setNodeMask(b ? ~static_cast<unsigned int>(0) : 0u);
        if (!b) beginFrame();
    }
}
