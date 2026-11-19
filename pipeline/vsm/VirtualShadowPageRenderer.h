#ifndef OSGVERSE_VIRTUAL_SHADOW_PAGE_RENDERER_H
#define OSGVERSE_VIRTUAL_SHADOW_PAGE_RENDERER_H

#include "VirtualShadowMap.h"
#include <osg/Camera>
#include <osg/Group>
#include <osg/Program>
#include <osg/Scissor>
#include <osg/Texture2D>
#include <vector>

namespace osgViewer { class View; }

namespace osgVerse
{
    /** Renders virtual shadow pages directly into the physical atlas.

        Each page camera owns a viewport and a scissor rectangle equal to its tile in the
        atlas, so the clear and the caster draw are confined to that tile. Rendering directly
        into the atlas avoids a separate copy pass and the texture-array FBO attachment that
        not every OSG/driver combination supports.

        The page cameras must be registered as dedicated view slaves (addToView()): a camera
        nested in the main scene graph would be traversed by every pipeline stage camera, and
        the stages whose cull mask excludes the casters would clear the tile without drawing
        it again, leaving the atlas empty.

        The caller drives it:
          1. pageRenderer->beginFrame()
          2. for each page to update:
             tile = pageRenderer->renderPage(vsm, key, view, projection, worldToShadow)
          3. draw the frame
        */
    class VirtualShadowPageRenderer : public osg::Referenced
    {
    public:
        VirtualShadowPageRenderer();

        bool configure(osg::GraphicsContext* gc, osg::Node* casterScene,
                       unsigned int casterMask, int concurrency,
                       const VirtualShadowSettings& settings, osg::Texture2D* atlas,
                       int glVersion, int glslVersion);

        /** Add the page cameras as view slaves, in front of the pipeline stages. */
        void addToView(osgViewer::View* view);

        osg::Group* getRoot() { return _root.get(); }
        const osg::Group* getRoot() const { return _root.get(); }
        int getConcurrency() const { return (int)_cameras.size(); }
        osg::Camera* getPageCamera(int index)
        { return (index >= 0 && index < (int)_cameras.size()) ? _cameras[index].get() : NULL; }

        void beginFrame();

        /** Render one page. 'view' and 'projection' place the page camera, and
            'worldToShadow' is the scale-bias * projection * view matrix stored per tile.
            Returns the tile slot, or -1 when the frame's concurrency is exhausted. */
        int renderPage(VirtualShadowMap* vsm, const VirtualShadowPageKey& key,
                       const osg::Matrixd& view, const osg::Matrixd& projection,
                       const osg::Matrix& worldToShadow);

        void finalize();
        void setEnabled(bool b);
        bool getEnabled() const { return _enabled; }

    protected:
        virtual ~VirtualShadowPageRenderer();
        osg::Camera* createPageCamera(int slot, int tileResolution, unsigned int casterMask,
                                      osg::GraphicsContext* gc, osg::Node* casterScene);

        osg::ref_ptr<osg::Group> _root;
        osg::ref_ptr<osg::Texture2D> _atlasTexture;
        osg::ref_ptr<osg::Program> _pageProgram;
        std::vector<osg::ref_ptr<osg::Camera>> _cameras;
        std::vector<osg::ref_ptr<osg::Scissor>> _scissors;
        unsigned int _casterMask;
        int _concurrency, _nextSlot, _tileResolution, _atlasResolution;
        int _renderOrderBase;
        bool _enabled;
    };
}

#endif
