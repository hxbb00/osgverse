#include <osg/io_utils>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geode>
#include <osg/PolygonMode>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>
#include <osg/FrameStamp>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgText/Text>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <pipeline/Pipeline.h>
#include <pipeline/LightModule.h>
#include <pipeline/LightCluster.h>
#include <pipeline/ShadowModule.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <ui/Utilities.h>
#include <iostream>
#include <random>
#include <sstream>

//#include <microprofile.h>
#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

namespace
{
    /** Create N random point/spot lights covering the scene bounds. They are just LightDrawable
        objects added to a Geode (exactly like the single main light), so that the existing
        culling callback of the light module collects them; no per-light shadow is needed here */
    void createRandomLights(osg::Geode* lightRoot, const osg::BoundingBox& bb, int numLights,
                            float rangeScale, float powerScale, float spotRatio, unsigned int seed)
    {
        std::mt19937 gen(seed);
        std::uniform_real_distribution<float> rnd(0.0f, 1.0f);
        osg::Vec3 ext = bb._max - bb._min;

        // Shrink the placement volume a bit so the lights stay inside the scene
        osg::Vec3 lo = bb._min + ext * 0.15f, hi = bb._max - ext * 0.15f;
        float maxExt = osg::maximum(ext.x(), osg::maximum(ext.y(), ext.z()));
        float baseRange = maxExt * rangeScale;

        for (int i = 0; i < numLights; ++i)
        {
            osg::ref_ptr<osgVerse::LightDrawable> light = new osgVerse::LightDrawable;
            osg::Vec3 pos(lo.x() + (hi.x() - lo.x()) * rnd(gen),
                          lo.y() + (hi.y() - lo.y()) * rnd(gen),
                          lo.z() + (hi.z() - lo.z()) * rnd(gen));
            light->setPosition(pos);

            float range = baseRange * (0.6f + 0.8f * rnd(gen));
            light->setRange(range);

            // The shader attenuates a light with an inverse-square term, so the power has to grow
            // with range * range for the lights to stay visible when the scene scale changes
            osg::Vec3 color(rnd(gen), rnd(gen), rnd(gen));
            color.normalize();
            color = color * (range * range * powerScale * (0.5f + 1.5f * rnd(gen)));
            light->setColor(color);

            osg::Vec3 dir(rnd(gen) - 0.5f, rnd(gen) - 0.5f, -(0.5f + rnd(gen)));  // mostly downward
            light->setDirection(dir);
            if (rnd(gen) < spotRatio) light->setSpotCutoff(0.35f + 0.55f * rnd(gen));  // radians
            lightRoot->addDrawable(light.get());
        }
    }

    /** Show the LightDrawable debug geometries as a wireframe overlay: a sphere for a point light,
        a cuboid for a directional light and a cone for a spot light (see
        LightDrawable::recreate()). It is meant for inspecting where the lights of a scene are */
    void showLightGeometries(osg::Geode* lightRoot, bool on, osg::PolygonMode* mode)
    {
        if (lightRoot == NULL) return;
        for (unsigned int i = 0; i < lightRoot->getNumDrawables(); ++i)
        {
            osgVerse::LightDrawable* ld =
                dynamic_cast<osgVerse::LightDrawable*>(lightRoot->getDrawable(i));
            if (ld != NULL) ld->setDebugShow(on);
        }

        osg::StateSet* ss = lightRoot->getOrCreateStateSet();
        if (on)
            ss->setAttributeAndModes(mode, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        else
            ss->removeAttribute(osg::StateAttribute::POLYGONMODE);
    }

    /** Simple on-screen text HUD showing the light count and the state of the light clustering.
        Keys 1-6 switch the whole rendering configuration (deferred/forward x lights x GI) so
        that every combination can be inspected and A/B-compared at runtime:
          1: forward,  no lights,    no GI      4: deferred, many lights, no GI
          2: forward,  many lights,  no GI      5: deferred, no lights,    GI
          3: deferred, no lights,    no GI      6: deferred, many lights,  GI */
    class LightStatHandler : public osgVerse::QuickEventHandler
    {
    public:
        LightStatHandler(osgVerse::LightModule* m, osgText::Text* text, int numLights,
                         osgVerse::Pipeline* p, osg::Node* sceneRoot,
                         osg::Shader* forwardVS, osg::Shader* forwardFS)
            : _module(m), _text(text), _numLights(numLights), _animated(true),
              _showDebug(false), _frame(0), _reported(false), _mode(0), _lightMask(0xffffffff),
              _giAvailable(true), _pipeline(p), _sceneRoot(sceneRoot),
              _forwardVS(forwardVS), _forwardFS(forwardFS)
        {
            _polygonMode = new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK,
                                                osg::PolygonMode::LINE);
            addKeyUpCallback('c', [this](int) { toggleCluster(); });
            addKeyUpCallback('C', [this](int) { toggleCluster(); });
            addKeyUpCallback('a', [this](int) { _animated = !_animated; });
            addKeyUpCallback('A', [this](int) { _animated = !_animated; });
            addKeyUpCallback('l', [this](int) { toggleDebug(); });
            addKeyUpCallback('L', [this](int) { toggleDebug(); });
            addKeyUpCallback('1', [this](int) { setMode(1); });
            addKeyUpCallback('2', [this](int) { setMode(2); });
            addKeyUpCallback('3', [this](int) { setMode(3); });
            addKeyUpCallback('4', [this](int) { setMode(4); });
            addKeyUpCallback('5', [this](int) { setMode(5); });
            addKeyUpCallback('6', [this](int) { setMode(6); });
        }

        void setLightRoot(osg::Geode* g)
        {
            _lightRoot = g; if (g != NULL) _lightMask = g->getNodeMask();
            // Snapshot the light colors once: the "no lights" mode zeroes them instead of
            // masking the geode out, because the light table is fed by the cull callback of
            // each LightDrawable and only pruned after several frames -- masking would leave
            // the stale lights lighting the scene long after the switch
            _savedColors.clear();
            if (g != NULL)
                for (unsigned int i = 0; i < g->getNumDrawables(); ++i)
                {
                    osgVerse::LightDrawable* ld =
                        dynamic_cast<osgVerse::LightDrawable*>(g->getDrawable(i));
                    _savedColors.push_back(ld ? ld->getColor() : osg::Vec3(0.0f, 0.0f, 0.0f));
                }
        }
        bool getShowDebug() const { return _showDebug; }
        void setShowDebug(bool b) { _showDebug = b; showLightGeometries(_lightRoot.get(), b, _polygonMode.get()); }

        bool getAnimated() const { return _animated; }
        void setAnimated(bool b) { _animated = b; }

        int getMode() const { return _mode; }
        void setMode(int m)
        {
            // Forward rendering has no deferred G-buffer at all, so the GI stages (which read
            // the deferred shadow/lighting buffers) are meaningless there: modes 1/2 are
            // no-GI by design
            if (m < 1 || m > 6) return;
            bool deferred = (m >= 3);
            bool lights = (m == 2 || m == 4 || m == 6);
            bool gi = deferred && (m >= 5);
            setLights(lights); applyGi(gi);
            applyDeferred(deferred);

            _mode = m;
            if (!deferred)
            {   // Known limitation of the standard pipeline: the forward pass draws to the back
                // buffer after the display stage, so its output skips tone mapping/exposure
                OSG_NOTICE << "[ManyLights] Mode " << m << ": forward rendering; its output is "
                           << "NOT tone mapped yet (see the pipeline TODO)" << std::endl;
            }
            updateText();
        }

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                _frame++;
                if ((_frame % 15) == 0) updateText();
                if (_frame == 30 && _module.valid())
                {   // Report once how many lights really reached the light table, which is the
                    // number the shader loops over in its flat (non-clustered) path
                    osg::Vec2 num(0.0f, 0.0f); _module->getLightNumber()->get(num);
                    OSG_NOTICE << "[ManyLights] " << (int)num.x() << " lights are active in the "
                               << "table (flat-pass cap " << (int)num.y() << ")" << std::endl;
                    
                    osg::View* view = dynamic_cast<osg::View*>(&aa);
                    if (view && view->getCamera())
                    {
                        osg::Vec3d eye, center, up;
                        view->getCamera()->getViewMatrixAsLookAt(eye, center, up);
                        OSG_NOTICE << "[ManyLights] Camera eye: (" << eye.x() << " " << eye.y() << " "
                                   << eye.z() << "), look-at: (" << center.x() << " " << center.y()
                                   << " " << center.z() << ")" << std::endl;
                    }
                    _reported = true;
                }
            }
            return osgVerse::QuickEventHandler::handle(ea, aa);
        }

        void updateText()
        {
            std::stringstream ss; ss << "Lights: " << _numLights;
            if (_module.valid())
            {
                osg::Vec2 num(0.0f, 0.0f); _module->getLightNumber()->get(num);
                const osgVerse::LightCluster* cluster = _module->getLightCluster();
                ss << " (active: " << (int)num.x() << ")"
                   << "\nLight cluster: " << (_module->getClusterEnabled() ? "ON" : "OFF");
                if (_module->getClusterEnabled() && cluster != NULL)
                {
                    const osg::Vec3i& g = cluster->getGridSize();
                    ss << "\nGrid: " << g.x() << "x" << g.y() << "x" << g.z() << " = "
                       << cluster->getNumClusters() << " clusters, " << cluster->getNumIndexEntries()
                       << " light refs";
                }
            }

            ss << "\nAnimation: " << (_animated ? "ON" : "OFF")
               << ", light wireframe: " << (_showDebug ? "ON" : "OFF");
            if (_mode >= 1)
            {
                bool gi = (_mode >= 5) && _giAvailable;
                ss << "\nRender mode " << _mode << ": "
                   << ((_mode >= 3) ? "deferred lighting, " : "forward rendering, ")
                   << ((_mode == 2 || _mode == 4 || _mode == 6) ? "many lights, " : "no lights, ")
                   << (gi ? "GI" : "no GI");
                if ((_mode >= 5) && !_giAvailable) ss << " (GI stages not built)";
            }

            ss << "\nKeys: [1-6] render mode, [C] cluster, [A] animation, [L] light wireframe";
            if (_text.valid())
            {
                _text->setText(ss.str());
                _text->dirtyDisplayList();
            }
        }

    protected:
        void toggleCluster()
        {
            if (_module.valid()) _module->setClusterEnabled(!_module->getClusterEnabled());
            updateText();
        }

        void toggleDebug()
        { setShowDebug(!_showDebug); updateText(); }

        void setLights(bool on)
        {
            if (!_lightRoot.valid() || _savedColors.empty()) return;
            for (unsigned int i = 0; i < _lightRoot->getNumDrawables() && i < _savedColors.size(); ++i)
            {
                osgVerse::LightDrawable* ld =
                    dynamic_cast<osgVerse::LightDrawable*>(_lightRoot->getDrawable(i));
                if (ld != NULL) ld->setColor(on ? _savedColors[i] : osg::Vec3(0.0f, 0.0f, 0.0f));
            }
        }

        void applyGi(bool on)
        {
            if (!_pipeline.valid()) return;
            osgVerse::Pipeline::Stage* ssgi = _pipeline->getStage("Ssgi");
            if (ssgi == NULL || ssgi->camera == NULL || _pipeline->getStage("SsgiBlur2") == NULL)
            {   // Built without GI (GLES2, debugShadowCombination or --no-ssgi): nothing to toggle
                _giAvailable = false; return;
            }
            osgVerse::Pipeline::Stage* blur1 = _pipeline->getStage("SsgiBlur1");
            osgVerse::Pipeline::Stage* blur2 = _pipeline->getStage("SsgiBlur2");
            osgVerse::Pipeline::Stage* tonemap = _pipeline->getStage("ToneMapping");
            osgVerse::Pipeline::Stage* luminance = _pipeline->getStage("LogLuminance");
            if (blur1 == NULL || blur1->camera == NULL || blur2->camera == NULL ||
                tonemap == NULL) { _giAvailable = false; return; }

            // A zero node mask makes the cull visitor skip the stage camera entirely, so the
            // disabled stages stop updating (and stop clearing) their buffers. The consumers
            // must then be pointed at a zero texel instead of the frozen GI buffer: coverage 0
            // makes the tone mapping fall back to the pure IBL term, which is exactly "no GI"
            unsigned int stageMask = on ? 0xffffffffu : 0x0u;
            ssgi->camera->setNodeMask(stageMask);
            blur1->camera->setNodeMask(stageMask);
            blur2->camera->setNodeMask(stageMask);
            if (on)
            {
                tonemap->applyBuffer(*blur2, "SsgiBlurredBuffer", 6);
                if (luminance != NULL)
                    luminance->applyBuffer(*blur2, "SsgiBlurredBuffer", "SsgiBuffer", 4);
            }
            else
            {
                tonemap->applyDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f), "SsgiBlurredBuffer", 6);
                if (luminance != NULL)
                    luminance->applyDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f), "SsgiBuffer", 4);
            }
        }

        void applyDeferred(bool on)
        {
            if (!_sceneRoot.valid()) return;
            osg::StateSet* ss = _sceneRoot->getOrCreateStateSet();
            ss->clear();  // also undoes the forward stateset of the previous mode
            if (on)
                { _sceneRoot->setNodeMask(DEFERRED_SCENE_MASK | SHADOW_CASTER_MASK); }
            else
            {
                if (!_forwardSS.valid() && _pipeline.valid())
                    _forwardSS = _pipeline->createForwardStateSet(_forwardVS.get(), _forwardFS.get());
                if (_forwardSS.valid()) ss->merge(*_forwardSS);
                _sceneRoot->setNodeMask(FORWARD_SCENE_MASK);
            }
        }

        osg::observer_ptr<osgVerse::LightModule> _module;
        osg::observer_ptr<osgText::Text> _text;
        osg::observer_ptr<osg::Geode> _lightRoot;
        osg::observer_ptr<osgVerse::Pipeline> _pipeline;
        osg::observer_ptr<osg::Node> _sceneRoot;
        osg::ref_ptr<osg::Shader> _forwardVS, _forwardFS;
        osg::ref_ptr<osg::StateSet> _forwardSS;
        osg::ref_ptr<osg::PolygonMode> _polygonMode;
        int _numLights; bool _animated, _showDebug, _reported; int _frame;
        int _mode; unsigned int _lightMask; bool _giAvailable;
        std::vector<osg::Vec3> _savedColors;
    };
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler);

    // The attenuation range is taken as a fraction of the scene size, so that the light pools stay
    // small compared to the scene (a range of the same order as the scene itself would light
    // everything and blow the near surfaces out). The power follows range * range because the
    // shader attenuates with an inverse-square term, which keeps a light well exposed at the
    // distance of its own range whatever the scene scale is
    float rangeScale = 0.12f; arguments.read("--light-range", rangeScale);
    float powerScale = 0.15f; arguments.read("--light-power", powerScale);
    float spotRatio = 0.35f; arguments.read("--spot-ratio", spotRatio);
    int numLights = 128; arguments.read("--lights", numLights);
    bool animate = !arguments.read("--no-animate");
    bool startNoCluster = arguments.read("--no-cluster");
    bool noSsao = arguments.read("--no-ssao");
    bool noSsgi = arguments.read("--no-ssgi");
    bool noBloom = arguments.read("--no-bloom");
    bool showLights = arguments.read("--show-lights");
    unsigned seed = 1234; arguments.read("--seed", seed);
    int clusterTile = 128; arguments.read("--cluster-tile", clusterTile);
    int clusterSlices = 16; arguments.read("--cluster-slices", clusterSlices);

    osg::ref_ptr<osg::Node> scene = osgDB::readNodeFiles(arguments);
    if (!scene) scene = osgDB::readNodeFile(BASE_DIR + "/models/Sponza.osgb");
    if (!scene) { OSG_WARN << "Failed to load any scene" << std::endl; return 1; }

    osg::ComputeBoundsVisitor cbv; scene->accept(cbv);
    osg::BoundingBox bb = cbv.getBoundingBox();
    if (!bb.valid()) { OSG_WARN << "Invalid scene bounds" << std::endl; return 1; }
    OSG_NOTICE << "[ManyLights] Scene bounds: (" << bb.xMin() << " " << bb.yMin() << " " << bb.zMin()
               << ") - (" << bb.xMax() << " " << bb.yMax() << " " << bb.zMax() << ")" << std::endl;

    // The scene graph
    osg::ref_ptr<osg::MatrixTransform> sceneRoot = new osg::MatrixTransform;
    sceneRoot->addChild(scene.get());
    osgVerse::Pipeline::setPipelineMask(*sceneRoot, DEFERRED_SCENE_MASK | SHADOW_CASTER_MASK);

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(sceneRoot.get());

    // Start the viewer with the standard (deferred + post effects) pipeline
    osg::ref_ptr<osgVerse::StandardPipelineViewer> viewer =
        new osgVerse::StandardPipelineViewer(true, false, false);
    viewer->getParameters().enableAO = !noSsao;  // SSAO uses a random kernel (noisy in captures)
    viewer->getParameters().enableGI = !noSsgi;  // one-bounce screen-space GI (same kind of noise)
    viewer->getParameters().enableBloom = !noBloom;  // bloom also drives the eye adaptation
    viewer->getParameters().shadowNumber = 0;  // no shadow needed: the lights are not shadow casters
    viewer->setSceneData(root.get());

    viewer->addEventHandler(new osgViewer::StatsHandler);
    viewer->addEventHandler(new osgViewer::WindowSizeHandler);
    viewer->addEventHandler(new osgGA::StateSetManipulator(viewer->getCamera()->getOrCreateStateSet()));
    viewer->setCameraManipulator(new osgGA::TrackballManipulator);
    viewer->setUpViewOnSingleScreen(0);
    viewer->realize();

    // The HUD canvas is created only after realize(), with the REAL framebuffer size: the
    // window may be DPI-scaled (a 1280x720 request on a 200% Windows display gives a
    // 2560x1600 framebuffer), and a hardcoded canvas size would map its content into just a
    // corner of the screen (with the previous 1920x1080 canvas the text could never reach
    // the top 1/3 of the display). The canvas ortho is 1:1 with framebuffer pixels, so the
    // text can be pinned to the top-left corner with exact coordinates
    int canvasW = 1920, canvasH = 1080;
    {   // Prefer the graphics context traits (framebuffer pixels); the master camera of the
        // pipeline viewer may have no context of its own (the stages render via slaves), in
        // which case its viewport is the best proxy for the real on-screen size
        osg::GraphicsContext* gc = viewer->getCamera()->getGraphicsContext();
        if (gc != NULL && gc->getTraits() != NULL)
            { canvasW = gc->getTraits()->width; canvasH = gc->getTraits()->height; }
        else
        {
            osg::Viewport* vp = viewer->getCamera()->getViewport();
            if (vp != NULL) { canvasW = (int)vp->width(); canvasH = (int)vp->height(); }
        }
        OSG_NOTICE << "[ManyLights] HUD canvas uses " << canvasW << "x" << canvasH << std::endl;
    }

    osgVerse::HeadUpDisplayCanvas hudCanvas;
    osg::Camera* hudCamera = hudCanvas.create(canvasW, canvasH);
    osgVerse::Pipeline::setPipelineMask(*hudCamera, FORWARD_SCENE_MASK);
    root->addChild(hudCamera);

    int hudFontSize = osg::maximum(26, (int)(canvasH * 0.03f));
    hudCanvas.createText("main", L"", hudFontSize, canvasW * 9 / 10, canvasH / 3, "root",
                         osgVerse::HeadUpDisplayCanvas::FREE,
                         osgVerse::HeadUpDisplayCanvas::BOTTOM);  // NOTE: maps to LAY_TOP of layout.h
    osgText::Text* hudText = hudCanvas.texts["main"].get();

    // The canvas layout puts the text block wherever its (y-down) rect happens to be; we want
    // it pinned to the top-left of the canvas instead, so set alignment and position directly.
    // (The anchor enum of the canvas does not match layout.h's internal flags -- BOTTOM there
    // means LAY_TOP -- which makes the createText anchor unreliable for multi-line HUD text.)
    if (hudText != NULL)
    {
        hudText->setAlignment(osgText::Text::LEFT_TOP);
        hudText->setPosition(osg::Vec3(0.02f * canvasW, 0.98f * canvasH, 0.0f));
    }

    // Configure the light module: TBO is the default, cluster culling is on by default
    osgVerse::Pipeline* pipeline = viewer->getPipeline();
    osgVerse::LightModule* lightModule =
        static_cast<osgVerse::LightModule*>(pipeline->getModule("Light"));
    if (lightModule == NULL) { OSG_WARN << "No light module found" << std::endl; return 1; }
    
    // The flat loop can show them all too. Note the +1: the main directional light already lives in
    // the light geode, so the table holds numLights + 1 entries, and a cap of numLights would make
    // the non-clustered path silently drop one light (the clustered one keeps it), which shows up
    // as a difference between the two paths as soon as that light lights something
    lightModule->setMaxLightsInPass(osg::maximum(24, numLights + 1));
    lightModule->setClusterEnabled(!startNoCluster);
    if (lightModule->getLightCluster())
    {
        lightModule->getLightCluster()->setTileSize(clusterTile);
        lightModule->getLightCluster()->setNumSlices(clusterSlices);
    }

    // The scene stays framed by the manipulator home position (a natural view of the whole scene);
    // no lateral or corner offset is applied. The agent options can still override the view
    osg::Geode* lightRoot = viewer->getLightRoot();

    // The global directional light is dimmed to a fill level (it can even be turned off): the
    // point/spot lights created below are the subject of this example. A small range is given to
    // it so that its debug cuboid has a visible size when --show-lights is used; the negligible
    // color keeps it from lighting the scene or casting a shadow
    if (lightModule->getMainLight() != NULL)
    {
        osg::Vec3 ext = bb._max - bb._min;
        lightModule->getMainLight()->setColor(osg::Vec3(0.02f, 0.02f, 0.02f));
        lightModule->getMainLight()->setRange(
            osg::maximum(ext.x(), osg::maximum(ext.y(), ext.z())) * 0.15f);
    }

    // Place the many lights in the existing light geode (index 0 is the main directional light)
    createRandomLights(lightRoot, bb, numLights, rangeScale, powerScale, spotRatio, seed);

    // HUD handler owns the text content and the hot keys, including the light wireframe toggle
    // and the 1-6 rendering-mode switching (deferred/forward x lights x GI)
    osg::ref_ptr<LightStatHandler> statHandler =
        new LightStatHandler(lightModule, hudText, numLights, pipeline, sceneRoot.get(),
                             viewer->getParameters().shaders.forwardVS.get(),
                             viewer->getParameters().shaders.forwardFS.get());
    statHandler->setLightRoot(lightRoot);
    statHandler->setAnimated(animate);
    statHandler->setShowDebug(showLights);

    int startMode = noSsgi ? 4 : 6;  // the configuration the startup flags produce
    arguments.read("--mode", startMode);
    statHandler->setMode(startMode);
    statHandler->updateText();
    viewer->addEventHandler(statHandler.get());
    if (showLights)
    {
        OSG_NOTICE << "[ManyLights] Light debug geometries are shown as a wireframe: sphere = "
                   << "point light, cuboid = directional light, cone = spot light" << std::endl;
    }

    // Agent automation (optional): set the view, capture and quit. This is a no-op unless one of
    // the --agent* options is given (see configureViewerForAgent in pipeline/Utilities.h)
    osgVerse::configureViewerForAgent(arguments, viewer.get());
    std::cout << "Many-lights testing started (num=" << numLights << ", cluster="
              << (lightModule->getClusterEnabled() ? "on" : "off") << ")..." << std::endl;

    osg::ref_ptr<osg::FrameStamp> lastStamp; double time0 = 0.0;
    while (!viewer->done())
    {
        if (statHandler->getAnimated())
        {
            if (time0 == 0.0) time0 = viewer->getFrameStamp()->getSimulationTime();
            double t = viewer->getFrameStamp()->getSimulationTime() - time0;

            // Move the extra lights (index 0 is the main light, which the manipulator/viewer owns)
            for (unsigned int i = 1; i < lightRoot->getNumDrawables(); ++i)
            {
                osgVerse::LightDrawable* ld =
                    dynamic_cast<osgVerse::LightDrawable*>(lightRoot->getDrawable(i));
                if (!ld) continue;
                
                osg::Vec3 p = ld->getPosition();
                p.x() += 0.02f * (float)cos(t * 1.7 + i);
                p.y() += 0.02f * (float)sin(t * 1.3 + i * 2);
                ld->setPosition(p);
            }
        }
        viewer->frame();
    }
    return 0;
}
