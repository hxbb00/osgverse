//
// osgVerse on WebGPU: the smallest possible scene.
//
// Draws an osg::Geometry triangle (or, when WEBGPU_SIMPLE_MODEL is defined, an
// online glTF model) through
//
//     osgViewer -> GraphicsWindowWGPU -> EGL -> gles-on-webgpu -> WebGPU
//
// instead of Emscripten's WebGL 2 emulation. Everything below is ordinary
// osgVerse/OSG code: the only WebGPU-specific statement is the windowing system
// preference on the traits.
//
// Build with -DWEBGPU_SIMPLE_MODEL="http://host/path/model.glb" to load a model;
// its textures use unit 0, which is what GraphicsWindowWGPU's context supports
// out of the box.
//

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osgGA/TrackballManipulator>
#include <osgDB/ReadFile>
#include <osgViewer/GraphicsWindow>
#include <osgViewer/Viewer>

#include <pipeline/Utilities.h>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <cmath>
#include <cstdio>
#include <string>

USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
// Registers osgVerse::GraphicsWindowWGPU under the "WGPU" windowing system name.
USE_GRAPICSWINDOW_IMPLEMENTATION(WGPU)

namespace
{

constexpr int kWidth  = 800;
constexpr int kHeight = 600;

osg::ref_ptr<osgViewer::Viewer> gViewer;
int gFrameCount = 0;

// --------------------------------------------------------------- scene -----
osg::Node *CreateTriangle()
{
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    vertices->push_back(osg::Vec3(0.0f, 0.7f, 0.0f));
    vertices->push_back(osg::Vec3(-0.65f, -0.5f, 0.0f));
    vertices->push_back(osg::Vec3(0.65f, -0.5f, 0.0f));

    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    colors->push_back(osg::Vec4(1.0f, 0.35f, 0.25f, 1.0f));
    colors->push_back(osg::Vec4(0.25f, 1.0f, 0.45f, 1.0f));
    colors->push_back(osg::Vec4(0.30f, 0.55f, 1.0f, 1.0f));

    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setVertexArray(vertices.get());
    geometry->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));

    // GLES 3 has no fixed-function pipeline, so even this needs a program.
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX, R"(#version 300 es
precision highp float;
in vec4 osg_Vertex;
in vec4 osg_Color;
uniform mat4 osg_ModelViewProjectionMatrix;
out vec4 vColor;
void main() { vColor = osg_Color; gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex; }
)"));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT, R"(#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)"));

    osg::StateSet *ss = geometry->getOrCreateStateSet();
    ss->setAttributeAndModes(program.get(), osg::StateAttribute::ON);
    ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(geometry.get());
    geode->setName("Triangle");
    return geode.release();
}

// Spins the MatrixTransform the triangle is attached to. It has to be attached
// to that node: a callback that casts its node to MatrixTransform must not be
// put on the Geode, or setMatrix() writes past the end of the object.
class SpinCallback : public osg::NodeCallback
{
public:
    void operator()(osg::Node *node, osg::NodeVisitor *nv) override
    {
        const double now = emscripten_get_now();
        if (mStart < 0.0) mStart = now;

        osg::MatrixTransform *transform = static_cast<osg::MatrixTransform *>(node);
        transform->setMatrix(osg::Matrix::rotate((now - mStart) / 1000.0 * 0.8, osg::Z_AXIS));
        traverse(node, nv);
    }

private:
    double mStart = -1.0;
};

// --------------------------------------------------------------- input -----
// osgVerse's SDL window feeds these through SDL; the canvas surface has no SDL,
// so the page's mouse events are translated here.
struct InputBridge
{
    osgViewer::Viewer *viewer = nullptr;
    int lastX = 0;
    int lastY = 0;
    int button = -1;

    void send(osgGA::GUIEventAdapter::EventType type, float x, float y, unsigned int mask = 0)
    {
        if (!viewer) return;
        osgGA::GUIEventAdapter *ea = new osgGA::GUIEventAdapter;
        ea->setEventType(type);
        ea->setX(x); ea->setY(y);
        ea->setXmin(0.0f); ea->setXmax(float(kWidth));
        ea->setYmin(0.0f); ea->setYmax(float(kHeight));
        ea->setButtonMask(mask);
        viewer->getEventQueue()->addEvent(ea);
    }

    void sendWheel(osgGA::GUIEventAdapter::ScrollingMotion motion, float x, float y)
    {
        if (!viewer) return;
        osgGA::GUIEventAdapter *ea = new osgGA::GUIEventAdapter;
        ea->setEventType(osgGA::GUIEventAdapter::SCROLL);
        ea->setScrollingMotion(motion);
        ea->setX(x); ea->setY(y);
        ea->setXmin(0.0f); ea->setXmax(float(kWidth));
        ea->setYmin(0.0f); ea->setYmax(float(kHeight));
        viewer->getEventQueue()->addEvent(ea);
    }
};

InputBridge gInput;

EM_BOOL OnMouse(int eventType, const EmscriptenMouseEvent *e, void *userData)
{
    const bool isDown = eventType == EMSCRIPTEN_EVENT_MOUSEDOWN;
    const bool isUp   = eventType == EMSCRIPTEN_EVENT_MOUSEUP;
    gInput.viewer = gViewer.get();
    gInput.lastX = e->canvasX; gInput.lastY = e->canvasY;

    unsigned int mask = 0;
    if (isDown) mask = (e->button == 2) ? osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON
                  : (e->button == 1) ? osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON
                                     : osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON;

    if (isDown)
    {
        gInput.button = (e->button == 2) ? 3 : (e->button == 1 ? 2 : 1);
        gInput.send(osgGA::GUIEventAdapter::PUSH, float(e->canvasX), float(e->canvasY), mask);
    }
    else if (isUp)
    {
        gInput.send(osgGA::GUIEventAdapter::RELEASE, float(e->canvasX), float(e->canvasY), mask);
    }
    else
    {
        gInput.send(osgGA::GUIEventAdapter::DRAG, float(e->canvasX), float(e->canvasY), mask);
    }
    return EM_TRUE;
}

EM_BOOL OnWheel(int, const EmscriptenWheelEvent *e, void *)
{
    gInput.viewer = gViewer.get();
    gInput.sendWheel(e->deltaY > 0 ? osgGA::GUIEventAdapter::SCROLL_UP
                                   : osgGA::GUIEventAdapter::SCROLL_DOWN,
                     float(e->mouse.canvasX), float(e->mouse.canvasY));
    return EM_TRUE;
}

// ---------------------------------------------------------------- loop -----
void Tick()
{
    if (!gViewer || gViewer->done())
    {
        emscripten_cancel_main_loop();
        return;
    }

    ++gFrameCount;
    const double t = emscripten_get_now() / 1000.0;
    gViewer->getCamera()->setClearColor(
        osg::Vec4(0.25f + 0.40f * float(0.5 + 0.5 * std::sin(t * 1.7)),
                  0.25f + 0.40f * float(0.5 + 0.5 * std::sin(t * 1.1 + 2.0)),
                  0.25f + 0.40f * float(0.5 + 0.5 * std::sin(t * 0.7 + 4.0)), 1.0f));

    gViewer->frame();

    if (gFrameCount == 1) std::printf("[wgpu_simple_demo] first frame submitted\n");
    else if (gFrameCount % 300 == 0) std::printf("[wgpu_simple_demo] frame %d\n", gFrameCount);
}

}  // namespace

int main(int argc, char **argv)
{
    osgVerse::globalInitialize(argc, argv);

    osg::ref_ptr<osg::Group> root = new osg::Group;
#ifdef WEBGPU_SIMPLE_MODEL
    osg::ref_ptr<osg::Node> scene = osgDB::readNodeFile(WEBGPU_SIMPLE_MODEL);
    if (scene.valid()) root->addChild(scene.get());
    else std::printf("[wgpu_simple_demo] could not read %s\n", WEBGPU_SIMPLE_MODEL);
#else
    osg::ref_ptr<osg::Node> triangle = CreateTriangle();
    osg::ref_ptr<osg::MatrixTransform> rotator = new osg::MatrixTransform;
    rotator->addChild(triangle.get());
    rotator->setUpdateCallback(new SpinCallback);
    root->addChild(rotator.get());
#endif

    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer->setSceneData(root.get());

    // OSG's default home position looks from -Y with +Z up, which puts a scene
    // drawn in the XY plane edge-on to the camera, so an explicit one is used.
    osg::ref_ptr<osgGA::TrackballManipulator> manipulator = new osgGA::TrackballManipulator;
    manipulator->setAutoComputeHomePosition(false);
    manipulator->setHomePosition(osg::Vec3d(0.0, 0.0, 2.6), osg::Vec3d(0.0, 0.0, 0.0),
                                 osg::Vec3d(0.0, 1.0, 0.0));
    viewer->setCameraManipulator(manipulator);

    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->x = 0; traits->y = 0; traits->width = kWidth; traits->height = kHeight;
    traits->alpha = 8; traits->depth = 24; traits->stencil = 8;
    traits->windowDecoration = false; traits->doubleBuffer = true;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->windowingSystemPreference = "WGPU";

    osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
    if (!gc.valid())
    {
        std::printf("[wgpu_simple_demo] could not create the WGPU graphics context\n");
        return 1;
    }

    osg::Camera *camera = viewer->getCamera();
    camera->setGraphicsContext(gc.get());
    camera->setViewport(0, 0, traits->width, traits->height);
    camera->setProjectionMatrixAsPerspective(45.0, double(traits->width) / traits->height, 0.1, 100.0);
    camera->setViewMatrixAsLookAt(osg::Vec3(0.0f, 0.0f, 2.6f), osg::Vec3(0.0f, 0.0f, 0.0f),
                                 osg::Vec3(0.0f, 1.0f, 0.0f));
    viewer->realize();
    gViewer = viewer;

    // The canvas texture taken while EGL was initialized belongs to this task;
    // submit whatever realize() recorded before the first animation frame.
    glFlush();

    gInput.viewer = viewer;
    // "#canvas" is the selector the WebGPU surface was created from; html5.h has
    // no EMSCRIPTEN_EVENT_TARGET_CANVAS, so the element is named directly.
    emscripten_set_mousedown_callback("#canvas", NULL, 1, OnMouse);
    emscripten_set_mouseup_callback("#canvas", NULL, 1, OnMouse);
    emscripten_set_mousemove_callback("#canvas", NULL, 1, OnMouse);
    emscripten_set_wheel_callback("#canvas", NULL, 1, OnWheel);

    emscripten_set_main_loop(Tick, 0, 0);
    return 0;
}
