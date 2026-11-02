#include <osg/io_utils>
#include <osg/CullFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Math>
#include <osg/ShapeDrawable>
#include <osgGA/GUIEventHandler>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgViewer/config/SingleWindow>
#include <iostream>

#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <pipeline/VolumetricCloud.h>
#include <VerseCommon.h>

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

// A cheap analytic sky used by the example. The volumetric cloud layer is independent of it:
// the earth test replaces this with the precomputed atmospheric scattering shaders.
static const char* demoSkyVS = {
    "VERSE_VS_OUT vec3 vDirection; \n"
    "void main() { \n"
    "    vDirection = normalize(osg_Vertex.xyz); \n"
    "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex; \n"
    "} \n"
};

static const char* demoSkyFS = {
    "uniform vec3 DemoSunDir, DemoSunColor; \n"
    "VERSE_FS_IN vec3 vDirection; \n"
    "VERSE_FS_OUT vec4 fragColor; \n"
    "void main() { \n"
    "    vec3 dir = normalize(vDirection); \n"
    "    float height = clamp(dir.z, 0.0, 1.0); \n"
    "    vec3 sky = mix(vec3(0.72, 0.80, 0.92), vec3(0.16, 0.38, 0.76), pow(height, 0.55)); \n"
    "    float mu = max(dot(dir, normalize(DemoSunDir)), 0.0); \n"
    "    sky += DemoSunColor * (pow(mu, 512.0) * 4.0 + pow(mu, 16.0) * 0.18); \n"
    "    fragColor = vec4(sky, 1.0); \n"
    "    VERSE_FS_FINAL(fragColor); \n"
    "} \n"
};

static const char* demoTerrainVS = {
    "VERSE_VS_OUT vec3 vNormal, vWorldPos; \n"
    "void main() { \n"
    "    vNormal = normalize(osg_Normal); \n"
    "    vWorldPos = osg_Vertex.xyz; \n"
    "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex; \n"
    "} \n"
};

static const char* demoTerrainFS = {
    "uniform vec3 DemoSunDir, DemoSunColor; \n"
    "VERSE_FS_IN vec3 vNormal, vWorldPos; \n"
    "VERSE_FS_OUT vec4 fragColor; \n"
    "void main() { \n"
    "    vec3 normal = normalize(vNormal); \n"
    "    float ndl = max(dot(normal, normalize(DemoSunDir)), 0.0); \n"
    "    float flatness = smoothstep(0.62, 0.92, normal.z); \n"
    "    vec3 grass = mix(vec3(0.18, 0.27, 0.13), vec3(0.36, 0.39, 0.23), flatness); \n"
    "    vec3 rock = vec3(0.36, 0.33, 0.30); \n"
    "    vec3 albedo = mix(rock, grass, flatness); \n"
    "    float snow = smoothstep(520.0, 760.0, vWorldPos.z) * flatness; \n"
    "    albedo = mix(albedo, vec3(0.82, 0.85, 0.88), snow * 0.85); \n"
    "    vec3 color = albedo * (0.34 + 1.05 * ndl) * DemoSunColor; \n"
    "    color += vec3(0.05, 0.07, 0.10) * albedo; \n"
    "    fragColor = vec4(color, 1.0); \n"
    "    VERSE_FS_FINAL(fragColor); \n"
    "} \n"
};

static float terrainHeight(float x, float y)
{
    float h = 330.0f * (std::sin(x * 0.0013f) * std::cos(y * 0.0011f)
                      + 0.5f * std::sin(x * 0.0031f + 1.7f) * std::cos(y * 0.0027f - 0.4f));
    h += 150.0f * std::sin(x * 0.0065f + 0.3f) * std::sin(y * 0.0072f - 0.8f);
    h += 60.0f * std::sin(x * 0.019f) * std::cos(y * 0.017f);
    return h + 380.0f;
}

static osg::Node* createTerrain(float extent, int resolution)
{
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    for (int j = 0; j < resolution; ++j)
    {
        float v = (float)j / (resolution - 1);
        float y = (v - 0.5f) * extent;
        for (int i = 0; i < resolution; ++i)
        {
            float u = (float)i / (resolution - 1);
            float x = (u - 0.5f) * extent;
            float z = terrainHeight(x, y);

            const float e = 4.0f;
            float hx0 = terrainHeight(x - e, y), hx1 = terrainHeight(x + e, y);
            float hy0 = terrainHeight(x, y - e), hy1 = terrainHeight(x, y + e);
            osg::Vec3 normal(hx0 - hx1, hy0 - hy1, 2.0f * e);
            normal.normalize();

            vertices->push_back(osg::Vec3(x, y, z));
            normals->push_back(normal);
        }
    }

    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    for (int j = 0; j < resolution - 1; ++j)
    {
        for (int i = 0; i < resolution - 1; ++i)
        {
            unsigned int a = j * resolution + i, b = a + 1;
            unsigned int c = a + resolution, d = c + 1;
            indices->push_back(a); indices->push_back(b); indices->push_back(d);
            indices->push_back(a); indices->push_back(d); indices->push_back(c);
        }
    }

    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
    geom->setVertexArray(vertices.get());
    geom->setNormalArray(normals.get(), osg::Array::BIND_PER_VERTEX);
    geom->addPrimitiveSet(indices.get());
    geom->setUseDisplayList(false);
    geom->setUseVertexBufferObjects(true);

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(geom.get());
    geode->setName("DemoTerrain");
    return geode.release();
}

static osg::Program* createDemoProgram(const char* vsCode, const char* fsCode, const char* name)
{
    osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, vsCode);
    osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, fsCode);
    int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
    osgVerse::Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
    osgVerse::Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(vs.get()); program->addShader(fs.get());
    program->setName(name);
    return program.release();
}

class ResizeProjectionHandler : public osgGA::GUIEventHandler
{
public:
    ResizeProjectionHandler(osg::Camera* cam, osg::Camera* hud, double fovY, double zn, double zf)
        : _camera(cam), _hud(hud), _fovY(fovY), _znear(zn), _zfar(zf), _synced(false) {}

    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
    {
        bool firstFrame = (ea.getEventType() == osgGA::GUIEventAdapter::FRAME && !_synced);
        if ((ea.getEventType() == osgGA::GUIEventAdapter::RESIZE || firstFrame) && _camera.valid())
        {
            int w = std::max(1, ea.getWindowWidth()), h = std::max(1, ea.getWindowHeight());
            if (ea.getWindowWidth() <= 0 || ea.getWindowHeight() <= 0)
            {
                osg::GraphicsContext* gc = _camera->getGraphicsContext();
                if (gc && gc->getTraits())
                {
                    w = std::max(1, gc->getTraits()->width);
                    h = std::max(1, gc->getTraits()->height);
                }
            }
            _camera->setProjectionMatrixAsPerspective(_fovY, (double)w / h, _znear, _zfar);
            // The scene/cloud buffers keep their allocation, but the HUD viewport has to
            // follow the window or the composite quad is displayed as a cropped region
            if (_hud.valid()) _hud->setViewport(0, 0, w, h);
            _synced = true;
        }
        return false;
    }

protected:
    osg::observer_ptr<osg::Camera> _camera, _hud;
    double _fovY, _znear, _zfar;
    bool _synced;
};

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    // Without --resolution the screen resolution is queried and the scene is shown in a
    // borderless full-screen window; --resolution switches back to a normal window
    int width = 1280, height = 720;
    bool customResolution = arguments.read("--resolution", width, height);
    if (!customResolution || width <= 0 || height <= 0)
    {
        unsigned int screenW = 0, screenH = 0;
        osgVerse::obtainScreenResolution(screenW, screenH);
        width = (int)(screenW ? screenW : 1280); height = (int)(screenH ? screenH : 720);
        customResolution = false;
    }

    int quality = (int)osgVerse::CloudRenderSettings::High;
    arguments.read("--quality", quality);
    quality = osg::clampBetween(quality, 0, 3);

    bool noClouds = arguments.read("--no-clouds");
    bool noDepth = arguments.read("--cloud-no-depth");
    bool noBilateral = arguments.read("--no-bilateral");
    bool freezeClouds = arguments.read("--freeze-clouds");
    bool temporalJitter = arguments.read("--cloud-jitter");
    int debugMode = 0; arguments.read("--cloud-debug", debugMode);
    int cloudType = (int)osgVerse::CloudLayerSettings::Cumulus;
    arguments.read("--cloud-type", cloudType);
    cloudType = osg::clampBetween(cloudType, 0, 4);
    float coverage = -1.0f, density = -1.0f;
    arguments.read("--cloud-coverage", coverage);
    arguments.read("--cloud-density", density);

    osg::Vec3 sunDir(0.35f, 0.55f, 0.42f); sunDir.normalize();
    arguments.read("--sun-dir", sunDir.x(), sunDir.y(), sunDir.z()); sunDir.normalize();

    // The cloud renderer owns its noise volumes, the ray-marching pass and the composite shader
    osg::ref_ptr<osgVerse::VolumetricCloud> clouds = new osgVerse::VolumetricCloud;
    clouds->setRenderSettings(osgVerse::CloudRenderSettings(
        (osgVerse::CloudRenderSettings::Quality)quality));
    clouds->setSunDirection(sunDir);
    clouds->setSunColor(osg::Vec3(1.55f, 1.48f, 1.36f));
    clouds->setAmbientColor(osg::Vec3(0.58f, 0.64f, 0.76f));
    clouds->setTemporalJitter(temporalJitter);
    if (freezeClouds) clouds->setTime(0.0f);
    {
        osgVerse::CloudLayerSettings layer((osgVerse::CloudLayerSettings::Type)cloudType);
        if (coverage >= 0.0f) layer.coverage = coverage;
        if (density >= 0.0f) layer.density = density;
        clouds->addLayer(layer);
    }

    // The opaque scene is rendered to an RTT color texture plus a depth texture, so that the
    // cloud pass can stop the rays at the terrain surface
    osg::ref_ptr<osg::Texture> sceneColor =
        osgVerse::Pipeline::createTexture(osgVerse::Pipeline::RGBA_INT8, width, height);
    osg::ref_ptr<osg::Texture2D> sceneDepth = osgVerse::VolumetricCloud::createDepthTexture(width, height);

    osg::Camera* sceneCamera = osgVerse::createRTTCamera(
        osg::Camera::COLOR_BUFFER0, sceneColor.get(), NULL, false);
    sceneCamera->setName("DemoScene");
    sceneCamera->setViewport(0, 0, width, height);
    sceneCamera->attach(osg::Camera::DEPTH_BUFFER, sceneDepth.get());

    // Procedural sky dome, drawn first and without writing depth
    {
        osg::ref_ptr<osg::Geode> skyGeode = new osg::Geode;
        osg::ref_ptr<osg::ShapeDrawable> sky = new osg::ShapeDrawable(
            new osg::Sphere(osg::Vec3(), 15000.0f), new osg::TessellationHints);
        sky->getOrCreateStateSet()->setAttributeAndModes(
            new osg::CullFace(osg::CullFace::FRONT), osg::StateAttribute::ON);
        sky->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        sky->getOrCreateStateSet()->setRenderBinDetails(-1, "RenderBin");
        sky->getOrCreateStateSet()->setAttributeAndModes(
            createDemoProgram(demoSkyVS, demoSkyFS, "DemoSky"));
        skyGeode->addDrawable(sky.get());
        sceneCamera->addChild(skyGeode.get());
    }

    // Terrain
    {
        osg::ref_ptr<osg::Node> terrain = createTerrain(12000.0f, 160);
        terrain->getOrCreateStateSet()->setAttributeAndModes(
            createDemoProgram(demoTerrainVS, demoTerrainFS, "DemoTerrain"));
        sceneCamera->addChild(terrain.get());
    }
    {
        osg::StateSet* ss = sceneCamera->getOrCreateStateSet();
        ss->addUniform(new osg::Uniform("DemoSunDir", sunDir));
        ss->addUniform(new osg::Uniform("DemoSunColor", osg::Vec3(1.0f, 0.96f, 0.88f)));
    }

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(sceneCamera);
    if (!noClouds)
    {
        clouds->createCloudPass(noDepth ? NULL : sceneDepth.get(), width, height);
        root->addChild(clouds.get());
    }

    // Composite the scene and the cloud buffer on a screen quad
    osg::Camera* displayCamera = osgVerse::createHUDCamera(
        NULL, width, height, osg::Vec3(), 1.0f, 1.0f, osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f), true, true);
    displayCamera->setName("DemoDisplay");
    osg::Texture* cloudBuffer = noClouds ? osgVerse::createDefaultTexture(
        osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f)) : clouds->getCloudBuffer();
    clouds->applyComposite(displayCamera->getOrCreateStateSet(), sceneColor.get(), cloudBuffer,
                           (noClouds || noBilateral) ? NULL : sceneDepth.get());
    displayCamera->getOrCreateStateSet()->getOrCreateUniform("DebugMode", osg::Uniform::INT)
                 ->set(debugMode);
    root->addChild(displayCamera);

    // Switch cloud presets at runtime: 1=Stratus 2=Stratocumulus 3=Cumulus 4=Cumulonimbus 5=Cirrus
    osg::ref_ptr<osgVerse::QuickEventHandler> handler = new osgVerse::QuickEventHandler;
    for (int i = 0; i < 5; ++i)
    {
        osgVerse::CloudLayerSettings::Type type = (osgVerse::CloudLayerSettings::Type)i;
        handler->addKeyDownCallback('1' + i, [clouds, type](int)
        {
            clouds->clearLayers();
            clouds->addLayer(osgVerse::CloudLayerSettings(type));
            OSG_NOTICE << "[CloudTest] Switched to cloud preset " << (int)type << std::endl;
        });
    }

    osgViewer::Viewer viewer;
    viewer.addEventHandler(handler.get());
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));

    osg::ref_ptr<osgGA::TrackballManipulator> manipulator = new osgGA::TrackballManipulator;
    manipulator->setHomePosition(osg::Vec3(0.0f, -9000.0f, 4500.0f),
                                 osg::Vec3(0.0f, 0.0f, 1500.0f), osg::Z_AXIS);
    viewer.setCameraManipulator(manipulator.get());
    viewer.setSceneData(root.get());
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);

    osg::ref_ptr<osgViewer::SingleWindow> windowConfig =
        new osgViewer::SingleWindow(0, 0, width, height, 0);
    windowConfig->setWindowDecoration(customResolution);
    viewer.apply(windowConfig.get());

    const double fovY = 55.0, znear = 1.0, zfar = 150000.0;
    double aspect = (double)width / std::max(1, height);
    viewer.getCamera()->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
    viewer.getCamera()->setProjectionMatrixAsPerspective(fovY, aspect, znear, zfar);
    viewer.addEventHandler(new ResizeProjectionHandler(
        viewer.getCamera(), displayCamera, fovY, znear, zfar));

    osgVerse::configureViewerForAgent(arguments, &viewer);
    return viewer.run();
}
