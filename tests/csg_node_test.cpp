#include <osg/io_utils>
#include <osg/ArgumentParser>
#include <osg/ColorMask>
#include <osg/CullSettings>
#include <osg/DisplaySettings>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/GL>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Math>
#include <osg/ShapeDrawable>
#include <osgDB/WriteFile>
#include <osgGA/GUIEventHandler>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <VerseCommon.h>
#include <pipeline/CSGNode.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <ui/Utilities.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

namespace
{
    // Unlit shaders used by the CSG primitives. The vertex shader transforms exactly like
    // the default vertex shader of the CSG computation, so that the depth values of the
    // computed surface and the ones of the shading pass match
    const char* objectVertCode = {
        "VERSE_VS_OUT vec4 color;\n"
        "void main() {\n"
        "    color = osg_Color;\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };

    const char* objectFragCode = {
        "VERSE_FS_IN vec4 color;\n"
        "VERSE_FS_OUT vec4 fragData;\n"
        "void main() {\n"
        "    fragData = color;\n"
        "    VERSE_FS_FINAL(fragData);\n"
        "}\n"
    };

    osg::Program* objectProgram()
    {
        static osg::ref_ptr<osg::Program> program;
        if (program.valid()) return program.get();

        int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
        osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, objectVertCode);
        osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, objectFragCode);
        osgVerse::Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
        osgVerse::Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

        program = new osg::Program;
        program->setName("csgNodeTest::object");
        program->addShader(vs.get()); program->addShader(fs.get());
        return program.get();
    }

    /// Geometry of one CSG primitive: a tessellated shape drawn with a flat color. Only the
    /// vertex positions matter for the boolean operation
    osg::ShapeDrawable* createShape(osg::Shape* shape, const osg::Vec4& color)
    {
        osg::ref_ptr<osg::TessellationHints> hints = new osg::TessellationHints;
        hints->setDetailRatio(1.0f);
        hints->setCreateNormals(false);
        hints->setCreateTextureCoords(false);

        osg::ShapeDrawable* drawable = new osg::ShapeDrawable(shape, hints.get());
        drawable->setName("csgNodeTest::shape");
        osg::Vec4Array* colors = new osg::Vec4Array;
        colors->assign(drawable->getVertexArray() ? drawable->getVertexArray()->getNumElements() : 0,
                       color);
        drawable->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        drawable->getOrCreateStateSet()->setAttributeAndModes(objectProgram());
        return drawable;
    }

    osg::Geode* createGeode(osg::Shape* shape, const osg::Vec4& color)
    {
        osg::Geode* geode = new osg::Geode;
        geode->addDrawable(createShape(shape, color));
        return geode;
    }

    /// Places the geometry with the given transformation ("scale * translate" scales first)
    osg::Node* transform(osg::Node* node, const osg::Matrix& matrix)
    {
        osg::MatrixTransform* transformNode = new osg::MatrixTransform(matrix);
        transformNode->addChild(node);
        return transformNode;
    }

    const osg::Vec4 colorA(1.0f, 0.45f, 0.20f, 1.0f);
    const osg::Vec4 colorB(0.25f, 0.60f, 1.00f, 1.0f);
    const osg::Vec4 colorC(0.45f, 0.90f, 0.35f, 1.0f);

    // -----------------------------------------------------------------------
    // shapes of the OpenCSG example application
    // -----------------------------------------------------------------------
    void buildSimple(osgVerse::CSGNode* csg)
    {
        csg->addPrimitive(createGeode(new osg::Sphere(osg::Vec3(-0.25f, 0.0f, 0.0f), 1.0f), colorA),
                          osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(createGeode(new osg::Sphere(osg::Vec3(0.25f, 0.0f, 0.0f), 1.0f), colorB),
                          osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(transform(
            createGeode(new osg::Sphere(osg::Vec3(), 1.0f), colorC),
            osg::Matrix::scale(0.5f, 0.5f, 2.0f) * osg::Matrix::translate(0.0f, 0.0f, 0.5f)),
            osgVerse::CSG_SUBTRACTION);
    }

    void buildWidget(osgVerse::CSGNode* csg)
    {
        csg->addPrimitive(createGeode(new osg::Sphere(osg::Vec3(), 1.2f), colorA),
                          osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(createGeode(new osg::Box(osg::Vec3(), 1.8f), colorB),
                          osgVerse::CSG_INTERSECTION);
        for (unsigned int axis = 0; axis < 3; ++axis)
        {
            osg::Matrix rotation;
            if (axis == 0) rotation = osg::Matrix::rotate(osg::inDegrees(90.0f), osg::Y_AXIS);
            if (axis == 1) rotation = osg::Matrix::rotate(osg::inDegrees(90.0f), osg::X_AXIS);
            csg->addPrimitive(transform(
                createGeode(new osg::Cylinder(osg::Vec3(), 0.6f, 2.5f), colorC),
                rotation * osg::Matrix::translate(0.0f, 0.0f, -1.25f)),
                osgVerse::CSG_SUBTRACTION);
        }
    }

    void buildGrid2D(osgVerse::CSGNode* csg)
    {
        csg->addPrimitive(createGeode(new osg::Box(osg::Vec3(0.0f, -0.25f, 0.0f),
                                                   2.5f, 0.5f, 2.5f), colorA),
                          osgVerse::CSG_INTERSECTION);
        for (int x = -2; x <= 2; ++x)
            for (int z = -2; z <= 2; ++z)
                csg->addPrimitive(createGeode(
                    new osg::Sphere(osg::Vec3(x * 0.5f, 0.0f, z * 0.5f), 0.22f), colorC),
                    osgVerse::CSG_SUBTRACTION);
    }

    void buildGrid3D(osgVerse::CSGNode* csg)
    {
        csg->addPrimitive(createGeode(new osg::Box(osg::Vec3(), 2.0f), colorA),
                          osgVerse::CSG_INTERSECTION);
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                    csg->addPrimitive(createGeode(
                        new osg::Sphere(osg::Vec3(float(x), float(y), float(z)), 0.58f), colorC),
                        osgVerse::CSG_SUBTRACTION);
    }

    void buildPipe(osgVerse::CSGNode* csg)
    {
        csg->addPrimitive(transform(
            createGeode(new osg::Cylinder(osg::Vec3(), 0.6f, 2.5f), colorA),
            osg::Matrix::translate(0.0f, 0.0f, -2.5f)), osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(transform(
            createGeode(new osg::Cylinder(osg::Vec3(), 0.5f, 2.52f), colorC),
            osg::Matrix::translate(0.0f, 0.0f, -2.51f)), osgVerse::CSG_SUBTRACTION);
        csg->addPrimitive(transform(
            createGeode(new osg::Cylinder(osg::Vec3(), 0.5f, 2.52f), colorC),
            osg::Matrix::rotate(osg::inDegrees(90.0f), osg::Y_AXIS)
                * osg::Matrix::translate(0.0f, 0.0f, -2.5f)), osgVerse::CSG_SUBTRACTION);
        csg->addPrimitive(transform(
            createGeode(new osg::Box(osg::Vec3(), 2.0f), colorC),
            osg::Matrix::translate(-0.8f, 1.0f, -0.8f)), osgVerse::CSG_SUBTRACTION);
    }

    typedef void (*ShapeBuilder)(osgVerse::CSGNode*);
    struct ShapeDescription
    {
        const char* name;
        ShapeBuilder builder;
        osg::Vec3 eye;
    };

    const ShapeDescription shapes[] =
    {
        { "Simple",  buildSimple, osg::Vec3(0.0f, 2.0f, 5.0f) },
        { "Widget",  buildWidget, osg::Vec3(0.0f, 2.0f, 5.0f) },
        { "2D-Grid", buildGrid2D, osg::Vec3(0.0f, 3.0f, 6.0f) },
        { "3D-Grid", buildGrid3D, osg::Vec3(0.0f, 3.0f, 7.0f) },
        { "Pipe",    buildPipe,   osg::Vec3(0.0f, 2.0f, 5.0f) }
    };
    const unsigned int numShapes = sizeof(shapes) / sizeof(shapes[0]);

    // -----------------------------------------------------------------------
    // interactive mode
    // -----------------------------------------------------------------------
    class CSGViewerHandler : public osgGA::GUIEventHandler
    {
    public:
        CSGViewerHandler(osgViewer::Viewer* viewer, osg::MatrixTransform* model,
                         osgVerse::HeadUpDisplayCanvas* hud)
        : _viewer(viewer), _model(model), _hud(hud), _csg(NULL), _shape(0), _spin(true),
          _inside(false), _bouncing(false), _angle(0.0f)
        { createShape(0); }

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                if (_spin)
                {
                    _angle += 0.5f;
                    if (_angle > 360.0f) _angle -= 360.0f;
                    _model->setMatrix(osg::Matrix::rotate(osg::inDegrees(_angle), osg::Y_AXIS));
                }
                if (_viewer->getFrameStamp()->getFrameNumber() % 60 == 0) printStatistics();
                return false;
            }
            if (ea.getEventType() != osgGA::GUIEventAdapter::KEYUP) return false;

            switch (ea.getKey())
            {
            case ' ': _spin = !_spin; break;
            case '1': case '2': case '3': case '4': case '5':
                createShape((unsigned int)(ea.getKey() - '1')); break;
            case 'i':
                _inside = !_inside; _csg->setCameraInsideModel(_inside);
                std::cout << "camera inside model: " << (_inside ? "yes" : "no") << std::endl;
                break;
            case 'b':
                _bouncing = !_bouncing;
                _csg->setSequencingMode(_bouncing ? osgVerse::CSG_BOUNCING_SEQUENCING
                                                  : osgVerse::CSG_SCHOENFIELD_SEQUENCING);
                std::cout << "sequencing: " << (_bouncing ? "bouncing" : "schoenfield")
                          << std::endl;
                break;
            case 's':
                _csg->setShadingPassEnabled(!_csg->getShadingPassEnabled());
                std::cout << "shading pass: " << (_csg->getShadingPassEnabled() ? "on" : "off")
                          << std::endl;
                break;
            case 'p': printStatistics(); break;
            default: break;
            }
            return false;
        }

    private:
        void createShape(unsigned int index)
        {
            if (index >= numShapes) return;
            if (_csg)
            {
                _model->removeChild(_csg);
                _csg = NULL;
            }

            _shape = index;
            _csg = new osgVerse::CSGNode;
            _csg->setStencilBits(8);
            _csg->setCameraInsideModel(_inside);
            _csg->setSequencingMode(_bouncing ? osgVerse::CSG_BOUNCING_SEQUENCING
                                              : osgVerse::CSG_SCHOENFIELD_SEQUENCING);
            shapes[index].builder(_csg);
            _model->addChild(_csg);

            osg::Vec3 eye = shapes[index].eye;
            _model->setMatrix(osg::Matrix::rotate(osg::inDegrees(_angle), osg::Y_AXIS));
            _viewer->getCameraManipulator()->setHomePosition(eye, osg::Vec3(), osg::Y_AXIS);
            _viewer->home();

            std::stringstream ss; ss << "Shape " << (index + 1) << "/" << numShapes
                                     << ": " << shapes[index].name
                                     << " (" << _csg->getNumPrimitives() << " primitives)";
            updateHud(ss.str());
            std::cout << "shape: " << shapes[index].name
                      << ", primitives: " << _csg->getNumPrimitives() << std::endl;
        }

        void updateHud(const std::string& shapeInfo)
        {
            if (_hud == NULL) return;
            std::map<std::string, osg::ref_ptr<osgText::Text>>::iterator it = _hud->texts.find("shape");
            if (it != _hud->texts.end()) it->second->setText(shapeInfo);
        }

        void printStatistics()
        {
            if (!_csg) return;
            std::cout << "  [" << shapes[_shape].name << "] primitives: "
                      << _csg->getNumPrimitives() << ", passes: " << _csg->getLastPassCount()
                      << ", frame: " << _viewer->getFrameStamp()->getFrameNumber() << std::endl;
        }

        osgViewer::Viewer* _viewer;
        osg::MatrixTransform* _model;
        osgVerse::HeadUpDisplayCanvas* _hud;
        osgVerse::CSGNode* _csg;
        unsigned int _shape;
        bool _spin, _inside, _bouncing;
        float _angle;
    };

    // -----------------------------------------------------------------------
    // frame buffer readback used by the self test and the capture mode
    // -----------------------------------------------------------------------
    class FrameGrabber : public osg::Camera::DrawCallback
    {
    public:
        explicit FrameGrabber(int captureFrame = -1)
        : _captureFrame(captureFrame), _frame(0), _captured(false), _captureRequested(false),
          _viewportX(0), _viewportY(0), _width(0), _height(0) {}

        void requestCapture() { _captured = false; _captureRequested = true; }

        virtual void operator()(osg::RenderInfo& renderInfo) const
        {
            ++_frame;
            if (!_captureRequested && (_captureFrame < 0 || _frame != _captureFrame)) return;
            if (_captured) return;

            osg::Camera* camera = renderInfo.getCurrentCamera();
            osg::Viewport* viewport = camera ? camera->getViewport() : NULL;
            if (viewport == NULL) return;

            _viewportX = (int)viewport->x(); _viewportY = (int)viewport->y();
            _width = (int)viewport->width(); _height = (int)viewport->height();
            if (_width <= 0 || _height <= 0) return;

            _depth.resize((std::size_t)_width * _height);
            _color.resize((std::size_t)_width * _height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(_viewportX, _viewportY, _width, _height,
                         GL_DEPTH_COMPONENT, GL_FLOAT, &_depth[0]);
            glReadPixels(_viewportX, _viewportY, _width, _height,
                         GL_RGBA, GL_UNSIGNED_BYTE, &_color[0]);
            _captured = true; _captureRequested = false;
        }

        bool captured() const { return _captured; }
        int width() const { return _width; }
        int height() const { return _height; }
        const std::vector<unsigned char>& pixels() const { return _color; }
        const std::vector<float>& depths() const { return _depth; }

        osg::Image* createImage() const
        {
            osg::Image* image = new osg::Image;
            unsigned char* data = new unsigned char[_color.size()];
            std::copy(_color.begin(), _color.end(), data);
            image->setImage(_width, _height, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE,
                            data, osg::Image::USE_NEW_DELETE);
            image->setOrigin(osg::Image::BOTTOM_LEFT);
            return image;
        }

        std::size_t index(double ndcX, double ndcY) const
        {
            int x = (int)((ndcX * 0.5 + 0.5) * (_width - 1));
            int y = (int)((ndcY * 0.5 + 0.5) * (_height - 1));
            x = osg::maximum(0, osg::minimum(x, _width - 1));
            y = osg::maximum(0, osg::minimum(y, _height - 1));
            return (std::size_t)y * _width + x;
        }

        float depth(double ndcX, double ndcY) const { return _depth[index(ndcX, ndcY)]; }

        osg::Vec4 color(double ndcX, double ndcY) const
        {
            const std::size_t i = index(ndcX, ndcY) * 4;
            return osg::Vec4(_color[i] / 255.0f, _color[i + 1] / 255.0f,
                             _color[i + 2] / 255.0f, _color[i + 3] / 255.0f);
        }

    private:
        int _captureFrame;
        mutable int _frame;
        mutable bool _captured, _captureRequested;
        mutable int _viewportX, _viewportY, _width, _height;
        mutable std::vector<float> _depth;
        mutable std::vector<unsigned char> _color;
    };

    // -----------------------------------------------------------------------
    // self test: verifies depth values and colors of one known configuration
    // -----------------------------------------------------------------------
    const double testFovY = 45.0, testZNear = 0.5, testZFar = 20.0, testEyeZ = 5.0;

    double windowDepth(double eyeDistance)
    {
        const double n = testZNear, f = testZFar, zEye = -eyeDistance;
        const double zNdc = (-(f + n) / (f - n) * zEye - 2.0 * f * n / (f - n)) / (-zEye);
        return (zNdc + 1.0) * 0.5;
    }

    class SelfTestHandler : public osgGA::GUIEventHandler
    {
    public:
        SelfTestHandler(osgViewer::Viewer* viewer, FrameGrabber* grabber)
        : _viewer(viewer), _grabber(grabber), _checked(false), _failures(0) {}

        int failures() const { return _failures; }

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
        {
            if (ea.getEventType() != osgGA::GUIEventAdapter::FRAME) return false;
            if (_grabber->captured() && !_checked)
            {
                _checked = true; check(); _viewer->setDone(true);
            }
            return false;
        }

    private:
        void check()
        {
            // cube [-1,1]^3 intersected with a sphere of radius 1.3 (which cuts the cube's
            // corners), a sphere of radius 0.6 at (0,0,1) is subtracted
            const double cutterSurface = windowDepth(testEyeZ - 0.4);  // back face of cutter
            const double frontFace = windowDepth(testEyeZ - 1.0);      // cube's front face
            std::cout << "\n=== osgVerse CSGNode self test ===\n"
                      << "expected depth of the cut surface: " << cutterSurface << "\n"
                      << "expected depth of the front face: " << frontFace << std::endl;

            checkDepth("cut surface (center)", 0.0, 0.0, cutterSurface);
            checkColor("cut surface (center)", 0.0, 0.0, colorC);
            checkDepth("front face", 0.3, 0.3, frontFace);
            checkColor("front face", 0.3, 0.3, colorA);
            checkDepth("background", 0.95, 0.95, 0.0);
            checkColor("background", 0.95, 0.95, osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            checkDepth("cut off corner", 0.543, 0.543, 0.0);
            checkColor("cut off corner", 0.543, 0.543, osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            std::cout << (_failures == 0 ? "PASSED\n" : "FAILED\n") << std::endl;
        }

        void checkDepth(const char* what, double x, double y, double expected)
        {
            const double actual = _grabber->depth(x, y), tolerance = 1.0e-3;
            if (std::fabs(actual - expected) > tolerance)
            {
                ++_failures;
                std::cout << "FAILED: depth of " << what << " at (" << x << ", " << y
                          << ") is " << actual << ", expected " << expected << std::endl;
            }
            else std::cout << "ok: depth of " << what << " is " << actual << std::endl;
        }

        void checkColor(const char* what, double x, double y, const osg::Vec4& expected)
        {
            const osg::Vec4 actual = _grabber->color(x, y);
            for (int i = 0; i < 3; ++i)
            {
                if (std::fabs(actual[i] - expected[i]) > 0.05)
                {
                    ++_failures;
                    std::cout << "FAILED: color of " << what << " at (" << x << ", " << y
                              << ") is " << actual << ", expected " << expected << std::endl;
                    return;
                }
            }
            std::cout << "ok: color of " << what << " is " << actual << std::endl;
        }

        osgViewer::Viewer* _viewer;
        FrameGrabber* _grabber;
        bool _checked;
        int _failures;
    };

    int runSelfTest()
    {
        osg::DisplaySettings::instance()->setMinimumNumStencilBits(8);

        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->getOrCreateStateSet()->setAttributeAndModes(new osg::ColorMask(true, true, true, true));

        osgVerse::CSGNode* csg = new osgVerse::CSGNode;
        csg->setName("csgNodeTest::selfTest");
        csg->addPrimitive(createGeode(new osg::Box(osg::Vec3(), 2.0f), colorA),
                          osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(createGeode(new osg::Sphere(osg::Vec3(), 1.3f), colorB),
                          osgVerse::CSG_INTERSECTION);
        csg->addPrimitive(createGeode(new osg::Sphere(osg::Vec3(0.0f, 0.0f, 1.0f), 0.6f), colorC),
                          osgVerse::CSG_SUBTRACTION);

        // Regression guard: a render bin of a primitive must not destroy the order of the
        // passes of the computation
        if (osg::Node* geometry = csg->getPrimitive(2)->getGeometry())
            geometry->getOrCreateStateSet()->setRenderBinDetails(10, "DepthSortedBin");
        root->addChild(csg);

        osgViewer::Viewer viewer;
        viewer.setUpViewInWindow(100, 100, 512, 512);
        viewer.setSceneData(root.get());
        viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        viewer.getCamera()->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        viewer.getCamera()->setClearMask(
            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        viewer.getCamera()->setProjectionMatrix(
            osg::Matrix::perspective(testFovY, 1.0, testZNear, testZFar));
        viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3(0.0, 0.0, testEyeZ), osg::Vec3(), osg::Y_AXIS);

        osg::ref_ptr<FrameGrabber> grabber = new FrameGrabber(3);
        viewer.getCamera()->setPostDrawCallback(grabber.get());
        osg::ref_ptr<SelfTestHandler> handler = new SelfTestHandler(&viewer, grabber.get());
        viewer.addEventHandler(handler.get());
        viewer.setCameraManipulator(NULL);

        while (!viewer.done()) viewer.frame();

        std::cout << "stencil bits of the frame buffer: "
                  << viewer.getCamera()->getGraphicsContext()->getTraits()->stencil
                  << ", passes: " << csg->getLastPassCount() << std::endl;
        return (handler->failures() == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // Renders one shape once and writes the frame buffer into the given image file
    int runCapture(const std::string& fileName, unsigned int shapeIndex)
    {
        if (shapeIndex >= numShapes) shapeIndex = 0;
        osg::DisplaySettings::instance()->setMinimumNumStencilBits(8);

        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->getOrCreateStateSet()->setAttributeAndModes(new osg::ColorMask(true, true, true, true));

        osg::ref_ptr<osgVerse::CSGNode> csg = new osgVerse::CSGNode;
        csg->setName("csgNodeTest::capture");
        shapes[shapeIndex].builder(csg.get());
        root->addChild(csg.get());

        osgViewer::Viewer viewer;
        viewer.setUpViewInWindow(100, 100, 512, 512);
        viewer.setSceneData(root.get());
        viewer.setCameraManipulator(NULL);
        viewer.getCamera()->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        viewer.getCamera()->setClearMask(
            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        osg::ref_ptr<FrameGrabber> grabber = new FrameGrabber;
        viewer.getCamera()->setPostDrawCallback(grabber.get());
        viewer.realize();
        csg->setStencilBits(viewer.getCamera()->getGraphicsContext()->getTraits()->stencil);

        viewer.getCamera()->setViewMatrixAsLookAt(shapes[shapeIndex].eye, osg::Vec3(), osg::Y_AXIS);
        for (int f = 0; f < 3; ++f) viewer.frame();
        grabber->requestCapture();
        for (int f = 0; f < 5 && !grabber->captured(); ++f) viewer.frame();

        if (!grabber->captured())
        {
            std::cout << "Capturing the frame buffer failed." << std::endl;
            return EXIT_FAILURE;
        }

        std::size_t covered = 0;
        const std::vector<unsigned char>& pixels = grabber->pixels();
        for (std::size_t p = 0; p < pixels.size() / 4; ++p)
        {
            const unsigned char maxComponent =
                (std::max)(pixels[p * 4], (std::max)(pixels[p * 4 + 1], pixels[p * 4 + 2]));
            if (maxComponent > 20) ++covered;
        }

        std::cout << shapes[shapeIndex].name << ": primitives " << csg->getNumPrimitives()
                  << ", passes " << csg->getLastPassCount() << ", covered "
                  << (100.0 * covered / (pixels.size() / 4)) << "%" << std::endl;
        if (!osgDB::writeImageFile(*grabber->createImage(), fileName))
        {
            std::cout << "Could not write " << fileName
                      << ", set OSG_LIBRARY_PATH to the osgPlugins directory" << std::endl;
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments =
        osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    if (arguments.read("--self-test") || arguments.read("--selftest"))
        return runSelfTest();

    std::string captureFile;
    int captureShape = 0;
    arguments.read("--shape", captureShape);
    if (arguments.read("--capture", captureFile))
        return runCapture(captureFile, captureShape);

    // Interactive viewer with the shapes of the OpenCSG example application:
    //   1..5 select shape, space starts/stops the rotation, 'i' toggles the camera inside
    //   the model, 'b' switches the sequencing, 's' enables/disables the shading pass,
    //   'p' prints statistics
    osg::DisplaySettings::instance()->setMinimumNumStencilBits(8);

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->getOrCreateStateSet()->setAttributeAndModes(new osg::ColorMask(true, true, true, true));
    osg::ref_ptr<osg::MatrixTransform> model = new osg::MatrixTransform;
    root->addChild(model);

    // On-screen hint text: the current shape is updated by the handler on key press
    typedef osgVerse::HeadUpDisplayCanvas HeadUpDisplayCanvas;
    osgVerse::HeadUpDisplayCanvas hudCanvas;
    root->addChild(hudCanvas.create(1920, 1080));
    hudCanvas.createText("shape", L"", 24, 800, 40, "root",
                         HeadUpDisplayCanvas::FREE, HeadUpDisplayCanvas::TOP);
    hudCanvas.createText("hint", L"Press [1]~[5] to switch shape, [Space] spin, [I] camera inside, "
                                 L"[B] sequencing, [S] shading pass, [P] statistics",
                         18, 1600, 40, "root",
                         HeadUpDisplayCanvas::FREE, HeadUpDisplayCanvas::BOTTOM);

    osgViewer::Viewer viewer(arguments);
    viewer.setSceneData(root.get());
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.getCamera()->setClearColor(osg::Vec4(0.15f, 0.15f, 0.18f, 1.0f));
    viewer.getCamera()->setClearMask(
        GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgViewer::ScreenCaptureHandler(
        new osgViewer::ScreenCaptureHandler::WriteToFile(
            "csg_node_screenshot", "png",
            osgViewer::ScreenCaptureHandler::WriteToFile::SEQUENTIAL_NUMBER)));
    viewer.addEventHandler(new CSGViewerHandler(&viewer, model.get(), &hudCanvas));
    return viewer.run();
}
