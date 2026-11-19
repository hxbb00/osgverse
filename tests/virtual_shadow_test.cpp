#include <osg/io_utils>
#include <osg/ComputeBoundsVisitor>
#include <osg/Math>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgDB/ReadFile>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <pipeline/Global.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <pipeline/vsm/VirtualShadowMap.h>
#include <pipeline/vsm/VirtualShadowPageRenderer.h>
#include <readerwriter/Utilities.h>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

namespace
{
    /** Diagnostic readback of the physical atlas after the copy pass. It writes a PGM file so
        the atlas content can be inspected without a debug HUD (the agent snapshot is taken at
        the forward pass, before a POST_RENDER HUD camera draws). */
    struct AtlasDumpCallback : public osg::Camera::DrawCallback
    {
        AtlasDumpCallback(osg::Texture2D* atlas, const std::string& file)
        :   _atlas(atlas), _file(file), _done(false) {}

        osg::observer_ptr<osg::Texture2D> _atlas;
        std::string _file; mutable bool _done;

        virtual void operator()(osg::RenderInfo& renderInfo) const
        {
            if (_done || !_atlas.valid()) return;
            _done = true;

            osg::State* state = renderInfo.getState();
            osg::Texture::TextureObject* obj =
                _atlas->getTextureObject(state->getContextID());
            if (obj == NULL)
            {
                std::cout << "[VSM Dump] No atlas texture object" << std::endl;
                return;
            }

            int w = _atlas->getTextureWidth(), h = _atlas->getTextureHeight();
            std::vector<float> data((size_t)w * h);
            glBindTexture(GL_TEXTURE_2D, obj->id());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, data.data());

            int step = osg::maximum(1, osg::maximum(w, h) / 512);
            int ow = (w + step - 1) / step, oh = (h + step - 1) / step;
            std::ofstream out(_file.c_str(), std::ios::binary);
            out << "P5\n" << ow << " " << oh << "\n255\n";
            for (int y = oh - 1; y >= 0; --y)
            {
                for (int x = 0; x < ow; ++x)
                {
                    float v = data[(size_t)(y * step) * w + x * step];
                    unsigned char b = (unsigned char)osg::clampBetween(
                        v * 255.0f, 0.0f, 255.0f);
                    out.write((const char*)&b, 1);
                }
            }
            std::cout << "[VSM Dump] Wrote " << _file << " (" << ow << "x" << oh << ")"
                      << std::endl;
        }
    };

    /** Diagnostic readback of the page table texture array (layer 0), colour-coded as in the
        debug HUD: dark blue = unmapped, cyan/orange = resident fine/coarse page. */
    struct PagesDumpCallback : public osg::Camera::DrawCallback
    {
        PagesDumpCallback(osg::Texture2DArray* pages, const std::string& file,
                          int tiling, int levels)
        :   _pages(pages), _file(file), _tiling(tiling), _levels(levels), _done(false) {}

        osg::observer_ptr<osg::Texture2DArray> _pages;
        std::string _file; int _tiling, _levels; mutable bool _done;

        virtual void operator()(osg::RenderInfo& renderInfo) const
        {
            if (_done || !_pages.valid()) return;
            _done = true;

            osg::State* state = renderInfo.getState();
            osg::Texture::TextureObject* obj =
                _pages->getTextureObject(state->getContextID());
            if (obj == NULL)
            {
                std::cout << "[VSM Dump] No page table texture object" << std::endl;
                return;
            }

            int w = _pages->getTextureWidth(), h = _pages->getTextureHeight();
            int layers = _pages->getTextureDepth();
            std::vector<float> data((size_t)w * h * layers * 4);
            glBindTexture(GL_TEXTURE_2D_ARRAY, obj->id());
            glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_FLOAT, data.data());

            std::ofstream out(_file.c_str(), std::ios::binary);
            out << "P6\n" << w << " " << h << "\n255\n";
            for (int y = h - 1; y >= 0; --y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const float* p = &data[((size_t)y * w + x) * 4];
                    unsigned char rgb[3];
                    if (p[3] < 0.5f)
                    { rgb[0] = 5; rgb[1] = 5; rgb[2] = 20; }
                    else
                    {
                        float level = p[2] / osg::maximum((float)(_levels - 1), 1.0f);
                        rgb[0] = (unsigned char)(255.0f * level);
                        rgb[1] = (unsigned char)(255.0f * (1.0f - level));
                        rgb[2] = 40;
                    }
                    out.write((const char*)rgb, 3);
                }
            }
            std::cout << "[VSM Dump] Wrote " << _file << " (" << w << "x" << h << ")"
                      << std::endl;
        }
    };

    osg::ref_ptr<osg::Node> createTestScene()
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;

        // Ground slab: its top surface is y = 0
        {
            osg::ref_ptr<osg::Geode> ground = new osg::Geode;
            osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(
                new osg::Box(osg::Vec3(0.0f, -0.5f, 0.0f), 200.0f, 1.0f, 200.0f));
            sd->setColor(osg::Vec4(0.55f, 0.55f, 0.58f, 1.0f));
            ground->addDrawable(sd.get());
            root->addChild(ground.get());
        }

        // A grid of boxes with varying heights, enough casters to show page streaming
        for (int x = -6; x <= 6; ++x)
        {
            for (int z = -6; z <= 6; ++z)
            {
                if (x == 0 && z == 0) continue;

                float h = 2.0f + (float)((x * 7 + z * 13 + 100) % 7);
                float w = 1.0f + 0.1f * (float)((x + z + 12) % 3);
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(
                    new osg::Box(osg::Vec3(x * 5.0f, h * 0.5f, z * 5.0f), w, h, w));
                sd->setColor(osg::Vec4(0.65f, 0.6f, 0.45f, 1.0f));
                geode->addDrawable(sd.get());
                root->addChild(geode.get());
            }
        }

        // A few tall pillars: their thin silhouettes make page seams and acne obvious
        for (int i = 0; i < 6; ++i)
        {
            float angle = osg::PI * 2.0f * (float)i / 6.0f;
            float x = cosf(angle) * 26.0f, z = sinf(angle) * 26.0f;
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(
                new osg::Box(osg::Vec3(x, 8.0f, z), 0.35f, 16.0f, 0.35f));
            sd->setColor(osg::Vec4(0.75f, 0.35f, 0.3f, 1.0f));
            geode->addDrawable(sd.get());
            root->addChild(geode.get());
        }

        osgVerse::Pipeline::setPipelineMask(*root,
            DEFERRED_SCENE_MASK | SHADOW_CASTER_MASK);
        return root.get();
    }

    osg::Program* createDebugProgram(const std::string& fragmentFile,
                                     int cxtVer, int glslVer)
    {
        osg::ref_ptr<osg::Shader> vs = osgDB::readShaderFile(
            osg::Shader::VERTEX, SHADER_DIR + "std_common_quad.vert.glsl");
        osg::ref_ptr<osg::Shader> fs = osgDB::readShaderFile(
            osg::Shader::FRAGMENT, SHADER_DIR + "vsm/" + fragmentFile);
        if (!vs.valid() || !fs.valid()) return NULL;

        osgVerse::Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
        osgVerse::Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(vs.get()); program->addShader(fs.get());
        return program.release();
    }

    osg::Geode* createDebugQuad(osg::Texture* texture, osg::Program* program,
                                const osg::Vec3& corner, float size,
                                const std::string& samplerName,
                                const osg::Vec2& pageParams)
    {
        if (program == NULL || texture == NULL) return NULL;

        osg::Geode* quad = osgVerse::createScreenQuad(
            corner, size, size, osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        osg::StateSet* ss = quad->getOrCreateStateSet();
        int value = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE;
        ss->setAttributeAndModes(program, value);
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        osgVerse::applyTextureAttributeAndModes(ss, 0, texture);
        ss->addUniform(new osg::Uniform(samplerName.c_str(), 0));
        if (pageParams.x() > 0.0f)
            ss->addUniform(new osg::Uniform("VsmDebugPageParams", pageParams));
        return quad;
    }
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(
        argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler);

    // The scene is both the visible scene and the caster source of the VSM pages
    osg::ref_ptr<osg::Group> root = new osg::Group;
    osg::ref_ptr<osg::Node> scene = createTestScene();
    root->addChild(scene.get());

    osg::ref_ptr<osgVerse::StandardPipelineViewer> viewer =
        new osgVerse::StandardPipelineViewer(false, true, false);
    viewer->getParameters().shadowNumber = 0;  // this test owns the shadow rendering
    viewer->getParameters().enableAO = false;
    viewer->getParameters().enablePostEffects = false;
    viewer->setSceneData(root.get());
    viewer->addEventHandler(new osgViewer::StatsHandler);
    viewer->addEventHandler(new osgViewer::WindowSizeHandler);

    osg::ref_ptr<osgGA::TrackballManipulator> manipulator = new osgGA::TrackballManipulator;
    manipulator->setHomePosition(osg::Vec3(55.0f, 45.0f, 55.0f),
                                 osg::Vec3(0.0f, 5.0f, 0.0f), osg::Vec3(0.0f, 1.0f, 0.0f));
    viewer->setCameraManipulator(manipulator.get());
    viewer->setUpViewOnSingleScreen(0);
    viewer->realize();

    // Main light direction: from the light towards the scene. The same direction drives the
    // virtual page cameras, so the lit image and the atlas preview agree
    osg::Vec3 lightDir(-0.35f, -1.0f, -0.45f); lightDir.normalize();
    viewer->setMainLight(osg::Vec3(1.5f, 1.45f, 1.35f), lightDir);

    osgVerse::Pipeline* pipeline = viewer->getPipeline();
    if (pipeline == NULL)
    {
        OSG_WARN << "[VSM Test] Standard pipeline is not available" << std::endl;
        return 1;
    }

    osgVerse::VirtualShadowSettings settings;
    settings.pageTableSize = 64;
    settings.maxLevels = 7;
    settings.tileResolution = 512;
    settings.maxTiles = 64;
    settings.maxPageRequestsPerFrame = 4;
    settings.maxLights = 1;
    settings.pagePadding = 2;
    settings.lodBias = 1.0f;

    osg::ref_ptr<osgVerse::VirtualShadowMap> vsm = new osgVerse::VirtualShadowMap;
    vsm->configure(settings, 1);

    osg::ref_ptr<osgVerse::VirtualShadowPageRenderer> pageRenderer =
        new osgVerse::VirtualShadowPageRenderer;
    if (!pageRenderer->configure(
            pipeline->getContext(), scene.get(), SHADOW_CASTER_MASK,
            settings.maxPageRequestsPerFrame, settings, vsm->getAtlasTexture(),
            pipeline->getContextTargetVersion(), pipeline->getGlslTargetVersion()))
    {
        OSG_WARN << "[VSM Test] Failed to create the page renderer" << std::endl;
        return 1;
    }
    root->addChild(pageRenderer->getRoot());

    std::string atlasDump;
    if (arguments.read("--dump-atlas", atlasDump))
    {
        // Attach to the last page camera so the dump runs after every page of the frame has
        // been written into the atlas
        int lastSlot = pageRenderer->getConcurrency() - 1;
        if (pageRenderer->getPageCamera(lastSlot) != NULL)
            pageRenderer->getPageCamera(lastSlot)->setPostDrawCallback(
                new AtlasDumpCallback(vsm->getAtlasTexture(), atlasDump));
    }

    std::string pagesDump;
    if (arguments.read("--dump-pages", pagesDump))
    {
        if (pageRenderer->getPageCamera(1) != NULL)
            pageRenderer->getPageCamera(1)->setPostDrawCallback(
                new PagesDumpCallback(vsm->getIndirectionTexture(), pagesDump,
                                      vsm->getTilePool()->getTilingCount(),
                                      settings.maxLevels));
    }

    // Debug HUD: atlas on the left, page table on the right
    // The pipeline detaches the main camera and presents through the last slave that owns a
    // graphics context. The debug HUD must draw into that same window context, otherwise its
    // output goes to an offscreen context that never reaches the screenshot
    osg::GraphicsContext* windowGC = pipeline->getContext();
    for (int i = (int)viewer->getNumSlaves() - 1; i >= 0; --i)
    {
        osg::Camera* slaveCam = viewer->getSlave(i)._camera.get();
        if (slaveCam != NULL && slaveCam->getGraphicsContext() != NULL)
        { windowGC = slaveCam->getGraphicsContext(); break; }
    }

    osg::ref_ptr<osg::Camera> hud = new osg::Camera;
    hud->setGraphicsContext(windowGC);
    hud->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    hud->setProjectionMatrix(osg::Matrix::ortho2D(0.0, 1.0, 0.0, 1.0));
    hud->setViewMatrix(osg::Matrix::identity());
    osg::Viewport* mainViewport = viewer->getCamera()->getViewport();
    hud->setViewport(0, 0, mainViewport ? (int)mainViewport->width() : 1920,
                     mainViewport ? (int)mainViewport->height() : 1080);
    hud->setRenderOrder(osg::Camera::POST_RENDER, 10000);
    hud->setClearMask(GL_DEPTH_BUFFER_BIT);
    hud->setDrawBuffer(GL_BACK);
    hud->setReadBuffer(GL_BACK);
    hud->setAllowEventFocus(false);

    int cxtVer = pipeline->getContextTargetVersion();
    int glslVer = pipeline->getGlslTargetVersion();
    osg::ref_ptr<osg::Program> atlasProgram =
        createDebugProgram("vsm_debug_atlas.frag.glsl", cxtVer, glslVer);
    osg::ref_ptr<osg::Program> pagesProgram =
        createDebugProgram("vsm_debug_pages.frag.glsl", cxtVer, glslVer);

    osg::Geode* atlasQuad = createDebugQuad(
        vsm->getAtlasTexture(), atlasProgram.get(),
        osg::Vec3(0.02f, 0.02f, 0.0f), 0.36f, "VsmDebugAtlas", osg::Vec2(-1.0f, -1.0f));
    osg::Geode* pagesQuad = createDebugQuad(
        vsm->getIndirectionTexture(), pagesProgram.get(),
        osg::Vec3(0.40f, 0.02f, 0.0f), 0.36f, "VsmDebugPages",
        osg::Vec2((float)vsm->getTilePool()->getTilingCount(), (float)settings.maxLevels));
    if (atlasQuad != NULL) hud->addChild(atlasQuad);
    if (pagesQuad != NULL) hud->addChild(pagesQuad);
    root->addChild(hud.get());

    // Stable light-space region. It is fixed in world/light space, which is what makes pages
    // world-anchored and reusable across frames
    osg::ComputeBoundsVisitor cbv;
    scene->accept(cbv);
    osg::BoundingBox bbf = cbv.getBoundingBox();
    osg::BoundingBoxd bb;
    for (int i = 0; i < 8; ++i)
        bb.expandBy(osg::Vec3d(bbf.corner(i).x(), bbf.corner(i).y(), bbf.corner(i).z()));
    const osg::Vec3d center = bb.center();
    const double radius = osg::maximum(bb.radius(), 1.0);
    const double regionSize = radius * 2.5;
    const double regionMinX = -regionSize * 0.5, regionMinY = -regionSize * 0.5;

    osg::Vec3d up = (fabs(lightDir * osg::Y_AXIS) > 0.9f)
                    ? osg::Vec3d(0.0, 0.0, 1.0) : osg::Vec3d(0.0, 1.0, 0.0);
    osg::Vec3d lightDirD(lightDir.x(), lightDir.y(), lightDir.z());
    osg::Vec3d eye = center - lightDirD * (radius * 3.0 + 1.0);
    osg::Matrixd lightView = osg::Matrixd::lookAt(eye, center, up);

    osg::BoundingBoxd lightSpaceBB;
    for (int i = 0; i < 8; ++i)
        lightSpaceBB.expandBy(bb.corner(i) * lightView);
    double nearPlane = osg::maximum(0.1, -lightSpaceBB.zMax() - 1.0);
    double farPlane = osg::maximum(nearPlane + 1.0, -lightSpaceBB.zMin() + 1.0);

    std::cout << "[VSM Test] Scene radius " << radius << ", region " << regionSize
              << ", light-space z [" << nearPlane << ", " << farPlane << "]" << std::endl;

    osgVerse::configureViewerForAgent(arguments, viewer.get());

    std::uint64_t frameNo = 0;
    while (!viewer->done())
    {
        vsm->beginFrame(frameNo);
        pageRenderer->beginFrame();
        vsm->getScheduler()->clear();

        // Focus page selection on the camera position, projected into light space
        osg::Vec3d cameraEye = viewer->getCamera()->getInverseViewMatrix().getTrans();
        osg::Vec3d focusLS = cameraEye * lightView;
        double fx = osg::clampBetween(focusLS.x(), regionMinX + 0.001,
                                      regionMinX + regionSize - 0.001);
        double fy = osg::clampBetween(focusLS.y(), regionMinY + 0.001,
                                      regionMinY + regionSize - 0.001);

        for (int level = 0; level < settings.maxLevels; ++level)
        {
            int grid = settings.pageTableSize >> level;
            if (grid < 1) grid = 1;

            double cell = regionSize / (double)grid;
            int cx = (int)floor((fx - regionMinX) / cell);
            int cy = (int)floor((fy - regionMinY) / cell);
            cx = osg::clampBetween(cx, 0, grid - 1);
            cy = osg::clampBetween(cy, 0, grid - 1);

            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= grid || y >= grid) continue;

                    // Coarser pages are more important during warm-up: they give the whole
                    // region a fallback before the fine pages stream in
                    osgVerse::VirtualShadowPageKey key(0, 0, level, x, y);
                    vsm->requestPage(key, (float)level + 1.0f);
                }
            }
        }

        std::vector<osgVerse::VirtualShadowScheduler::Request> requests;
        vsm->getScheduler()->take(settings.maxPageRequestsPerFrame, requests);
        for (size_t i = 0; i < requests.size(); ++i)
        {
            const osgVerse::VirtualShadowPageKey& key = requests[i].key;
            int grid = settings.pageTableSize >> key.level;
            if (grid < 1) grid = 1;

            double cell = regionSize / (double)grid;
            double xMin = regionMinX + key.x * cell;
            double yMin = regionMinY + key.y * cell;
            osg::Matrixd projection = osg::Matrixd::ortho(
                xMin, xMin + cell, yMin, yMin + cell, nearPlane, farPlane);
            osg::Matrixd bias = osg::Matrixd::translate(0.5, 0.5, 0.5) *
                                osg::Matrixd::scale(0.5, 0.5, 0.5);
            osg::Matrixd worldToShadow = bias * projection * lightView;

            int tile = pageRenderer->renderPage(
                vsm.get(), key, lightView, projection, worldToShadow);
            if (tile < 0)  // all page cameras are busy or the tile pool is full: retry
                vsm->requestPage(key, requests[i].priority);
        }

        vsm->updateGpuData();
        pageRenderer->finalize();
        vsm->endFrame();

        if ((frameNo % 60) == 0)
        {
            const osgVerse::VirtualShadowStats& stats = vsm->getStats();
            std::cout << "[VSM] frame " << frameNo
                      << " resident=" << stats.residentPages
                      << " pending=" << stats.pendingRequests
                      << " updated=" << stats.updatedTiles
                      << " evicted=" << stats.evictedTiles
                      << " hits=" << stats.cacheHits
                      << " misses=" << stats.cacheMisses << std::endl;
        }

        viewer->frame();
        ++frameNo;
    }
    return 0;
}
