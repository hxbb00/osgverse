#include <osg/io_utils>
#include <osg/ComputeBoundsVisitor>
#include <osg/DisplaySettings>
#include <osg/LightSource>
#include <osg/Texture2D>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgGA/StateSetManipulator>
#include <osgUtil/CullVisitor>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <pipeline/Pipeline.h>
#include <pipeline/LightModule.h>
#include <pipeline/NISUpscaler.h>
#include <pipeline/RenderCallbackXR.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>

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

class UpdateHandlerXR : public osgGA::GUIEventHandler
{
public:
    UpdateHandlerXR(osg::StateSet* ss, osgVerse::RenderCallbackXR* xr) : _sceneStateSet(ss), _xr(xr)
    {
        updateStageForStereoVR(osgDB::readShaderFile(
            osg::Shader::GEOMETRY, SHADER_DIR + "forward/std_forward_render.geom.glsl"), true);
    }

    bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        if (_xr.valid())
        {
            osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
            if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                osg::Matrix vMatrix, pMatrix;
                osg::Camera* cam = view->getCamera();

                _xr->handleEvents(view->getEventQueue());
                if (updateMatricesForStereoVR(vMatrix, pMatrix, cam->getProjectionMatrix()))
                {
                    // Also apply to main camera
                    osgGA::CameraManipulator* mani = view->getCameraManipulator();
                    if (mani) mani->setByInverseMatrix(vMatrix);
                    else cam->setViewMatrix(vMatrix);
                    cam->setProjectionMatrix(pMatrix);
                }

                osgVerse::RenderCallbackXR::HandInputState handL, handR;
                if (_xr->handleInputs(handL, handR))
                {
                    std::cout << "L" << handL.buttons["menu"] << ": " << handL.aimPose.getTrans() << "; ";
                    std::cout << "R" << handR.buttons["menu"] << ": " << handR.aimPose.getTrans() << "\n";
                }
            }
        }
        return false;
    }

    void updateStageForStereoVR(osg::Shader* geomShader, bool useClip)
    {
        int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
        osgVerse::Pipeline::createShaderDefinitions(geomShader, cxtVer, glslVer);

        osg::Program* prog = static_cast<osg::Program*>(
            _sceneStateSet->getAttribute(osg::StateAttribute::PROGRAM));
        if (prog && prog->getParameter(GL_GEOMETRY_VERTICES_OUT_EXT) <= 3)
        {
#if OSG_VERSION_GREATER_THAN(3, 3, 6)
            //prog->addDefinitions(osg::Shader::VERTEX, "#define VERSE_VRMODE 1");
            _sceneStateSet->setDefine("VERSE_VRMODE");
#endif
            prog->addShader(geomShader);
            prog->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT, 2 * 3);  // Left/Right
            prog->setParameter(GL_GEOMETRY_INPUT_TYPE_EXT, GL_TRIANGLES);
            prog->setParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT, GL_TRIANGLES);
        }

        if (useClip)
        {
#if !defined(OSG_GL3_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
            _sceneStateSet->setMode(GL_CLIP_PLANE0, osg::StateAttribute::ON);
            _sceneStateSet->setMode(GL_CLIP_PLANE1, osg::StateAttribute::ON);
            _sceneStateSet->setMode(GL_CLIP_PLANE2, osg::StateAttribute::ON);
            _sceneStateSet->setMode(GL_CLIP_PLANE3, osg::StateAttribute::ON);
#endif
        }
    }

    bool updateMatricesForStereoVR(osg::Matrix& view, osg::Matrix& proj, const osg::Matrix& proj0)
    {
        osg::Matrixf viewL, viewR, projL, projR;
        double znear = 0.0, zfar = 0.0; extractNearFar(proj0, znear, zfar);
        if (_xr->begin(viewL, viewR, projL, projR, znear, zfar))
        {
            osg::Uniform* v = _sceneStateSet->getOrCreateUniform("viewMatrices", osg::Uniform::FLOAT_MAT4, 2);
            osg::Uniform* p = _sceneStateSet->getOrCreateUniform("projMatrices", osg::Uniform::FLOAT_MAT4, 2);
            v->setElement(0, viewL); v->setElement(1, viewR); p->setElement(0, projL); p->setElement(1, projR);
            view = viewL; proj = projL; return true;
        }
        return false;
    }

    void extractNearFar(const osg::Matrix& proj, double& znear, double& zfar)
    {
        double A = proj(2, 2), B = proj(3, 2);
        znear = B / (A - 1.0); zfar = B / (A + 1.0);
    }

protected:
    osg::observer_ptr<osg::StateSet> _sceneStateSet;
    osg::observer_ptr<osgVerse::RenderCallbackXR> _xr;
};

osg::StateSet* createPbrStateSet(osgVerse::Pipeline* pipeline)
{
    osg::ref_ptr<osg::StateSet> forwardSS = pipeline->createForwardStateSet(
        osgDB::readShaderFile(osg::Shader::VERTEX, SHADER_DIR + "forward/std_forward_render.vert.glsl"),
        osgDB::readShaderFile(osg::Shader::FRAGMENT, SHADER_DIR + "forward/std_forward_render.frag.glsl"));

    osgVerse::LightModule* lm = static_cast<osgVerse::LightModule*>(pipeline->getModule("Light"));
    if (forwardSS.valid() && lm)
    {
        // Bind the parameter table and (on TBO platforms) the cluster buffers to the forward
        // stateset. refreshCam is NULL because this test has no pipeline stage camera: the
        // LightModule itself is an update callback of the viewer camera and keeps the table
        // (and the clusters, which the forward shader also walks now) up to date instead
        lm->applyToStateSet(&*forwardSS, NULL, "LightParameterMap", 7);
    }
    return forwardSS.release();
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler);

    // NIS upscaling: the scene is rendered at a smaller resolution and upscaled to the window
    bool nisEnabled = arguments.read("--nis");
    int nisWidth = 0, nisHeight = 0;
    arguments.read("--nis-size", nisWidth, nisHeight);
    float nisSharpness = 0.5f; arguments.read("--nis-sharpness", nisSharpness);
    if (nisEnabled && arguments.find("--openxr") >= 0)
    {
        OSG_WARN << "[NIS] NIS upscaling is disabled in OpenXR mode" << std::endl;
        nisEnabled = false;
    }
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
    if (nisEnabled)
    {
        OSG_WARN << "[NIS] NIS upscaling is only supported by desktop OpenGL" << std::endl;
        nisEnabled = false;
    }
#else
    // Compute shaders used by NIS require a GL 4.3+ context. The GL2 build still uses
    // fixed-function states, which are only valid in a compatibility profile: requesting a 4.x
    // context without a profile mask may return a core context and flood the log with GL errors
    if (nisEnabled)
    {
        osg::DisplaySettings* ds = osg::DisplaySettings::instance();
        ds->setGLContextVersion("4.3");
#if !defined(OSG_GL3_AVAILABLE)
        ds->setGLContextProfileMask(0x00000002);  // GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#endif
    }
#endif

    // Only pass real local files to osgDB::readNodeFiles(): it treats every non-option token as
    // a file name, which would eat the values of options such as --nis-size or --agent-eye
    std::vector<std::string> sceneFiles;
    for (int i = 1; i < arguments.argc(); ++i)
    {
        std::string name = arguments[i];
        if (!osg::ArgumentParser::isOption(name.c_str()) && osgDB::fileExists(name))
            sceneFiles.push_back(name);
    }
    osg::ref_ptr<osg::Node> scene =
        sceneFiles.empty() ? NULL : osgDB::readNodeFiles(sceneFiles);
    if (!scene) scene = osgDB::readNodeFile(BASE_DIR + "/models/Sponza/Sponza.gltf.125,125,125.scale");
    if (!scene) { OSG_WARN << "Failed to load scene model"; return 1; }

    // The scene graph
    osg::ref_ptr<osg::MatrixTransform> sceneRoot = new osg::MatrixTransform;
    sceneRoot->addChild(scene.get());

    osg::ref_ptr<osgVerse::LightDrawable> light0 = new osgVerse::LightDrawable;
    light0->setColor(osg::Vec3(1.0f, 1.0f, 1.0f));
    light0->setDirection(osg::Vec3(0.02f, 0.1f, -1.0f));
    light0->setDirectional(true);

    osg::ref_ptr<osgVerse::LightDrawable> light1 = new osgVerse::LightDrawable;
    light1->setColor(osg::Vec3(1.0f, 1.0f, 1.0f));
    light1->setDirection(osg::Vec3(-0.4f, 0.6f, 0.1f));
    light1->setDirectional(true);

    osg::ref_ptr<osg::Geode> lightGeode = new osg::Geode;
    lightGeode->addDrawable(light0.get());  // Main light
    lightGeode->addDrawable(light1.get());

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(sceneRoot.get());
    root->addChild(lightGeode.get());

    // The pipeline only for shader construction and lighting. It must know the real target GL/GLSL
    // versions before any shader is created: with the default (100/120) the generated definitions
    // use the fixed-function aliases (gl_Vertex, gl_ModelViewProjectionMatrix...), which do not
    // exist in a GL3 core or GLES3 context, so the forward shader would fail to compile there
    // (e.g. a completely black scene in the GLES3 build)
    int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
    osg::ref_ptr<osgVerse::Pipeline> pipeline = new osgVerse::Pipeline(cxtVer, glslVer);
    osg::ref_ptr<osgVerse::LightModule> lightModule = new osgVerse::LightModule("Light", pipeline.get());
    lightModule->setMainLight(light0.get(), "");  // no shadow module
    sceneRoot->setStateSet(createPbrStateSet(pipeline.get()));

    // Start the viewer
    osgViewer::Viewer viewer;
    viewer.getCamera()->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    viewer.getCamera()->addUpdateCallback(lightModule.get());
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);

    if (arguments.read("--openxr"))
    {
        // Optional, VR mode
#if false
        // Example code to set-up PICO4 interaction profiles
        osgVerse::RenderCallbackXR* xr = new osgVerse::RenderCallbackXR({ "XR_BD_controller_interaction", "XR_BD_ultra_controller_interaction" });

        // https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#_bytedance_pico_4_controller_profile
        #define IN_ACTION osgVerse::RenderCallbackXR::InputActionDefinition
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::BOOLEAN, "primary", "Button X/A", "/input/x/touch", "/input/a/touch"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::BOOLEAN, "secondary", "Button Y/B", "/input/y/touch", "/input/b/touch"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::BOOLEAN, "menu", "Menu Button", "/input/menu/click", ""));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::FLOAT, "trigger", "Trigger", "/input/trigger/value", "/input/trigger/value"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::FLOAT, "squeeze", "Squeeze", "/input/squeeze/value", "/input/squeeze/value"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::FLOAT, "thumbstick_x", "Thumbstick X", "/input/thumbstick/x", "/input/thumbstick/x"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::FLOAT, "thumbstick_y", "Thumbstick Y", "/input/thumbstick/y", "/input/thumbstick/y"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::POSE, "aim_pose", "Aim Pose", "/input/aim/pose", "/input/aim/pose"));
        xr->addInputActionDefinition(IN_ACTION(IN_ACTION::POSE, "grip_pose", "Grip Pose", "/input/grip/pose", "/input/grip/pose"));
        xr->addSuggestedInteractionProfile("/interaction_profiles/bytedance/pico4_controller");
#else
        osgVerse::RenderCallbackXR* xr = new osgVerse::RenderCallbackXR;
#endif
        xr->setup(viewer.getCamera(), 2);  // post-draw callback to endFrame() and submit frame buffer to VR
        viewer.addEventHandler(new UpdateHandlerXR(sceneRoot->getStateSet(), xr));  // frame event to beginFrame()

        // Move the model far from the HMD position to make it visible at first
        sceneRoot->setMatrix(osg::Matrix::translate(0.0f, 0.0f, -scene->getBound().radius()));
    }

    int screenNo = 0; arguments.read("--screen", screenNo);
    viewer.setUpViewOnSingleScreen(screenNo);

    // NIS upscaling: the scene is rendered by an offscreen camera into a small FBO, upscaled by
    // the NIS compute shader and presented by a screen quad. The interactive main camera is kept
    // for the manipulator only, so the user interaction does not change
    osgVerse::NISUpscalingView nisView;
    if (nisEnabled)
    {
        // The window (and its context) has to exist before sizing the NIS render targets
        viewer.realize();
        osg::Camera* mainCamera = viewer.getCamera();
        const osg::GraphicsContext::Traits* traits = mainCamera->getGraphicsContext()
                                                   ? mainCamera->getGraphicsContext()->getTraits()
                                                   : NULL;
        int winW = (traits && traits->width > 0) ? traits->width : 1280;
        int winH = (traits && traits->height > 0) ? traits->height : 720;
        int inW = (nisWidth > 0 && nisHeight > 0) ? nisWidth : (winW + 1) / 2;
        int inH = (nisWidth > 0 && nisHeight > 0) ? nisHeight : (winH + 1) / 2;
        inW = (std::max)(1, (std::min)(inW, winW));
        inH = (std::max)(1, (std::min)(inH, winH));

        // NIS scaler only supports a scale of [0.5, 1.0], so the output is at most 2x the input
        int outW = (std::min)(winW, inW * 2), outH = (std::min)(winH, inH * 2);
        nisView = osgVerse::NISUpscaler::createUpscalingView(
            &viewer, root.get(), inW, inH, outW, outH, nisSharpness);
        if (!nisView.valid())
        {
            OSG_WARN << "[NIS] Failed to create the upscaling chain, direct rendering is used"
                     << std::endl;
            nisEnabled = false;
        }
    }

    // Optional automation for AI agent / scripted testing: set the view, capture and quit
    osgVerse::configureViewerForAgent(arguments, &viewer);
    return viewer.run();
}
