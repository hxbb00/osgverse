/* Gaussian splatting viewer and simplification test.

   Usage: osgVerse_Test_3DGS <model> [options]

   Model options (any format supported by the verse_3dgs plugin: .ply, .splat, .spz,
   .ksplat, .sog, .lcc, ...):
     -O <string>                 Extra reader options, e.g. "RenderMethod=0".
     --render-mode <hint>        Custom-mode render method: "" (SSBO instancing), "TBO", "TEX2D",
                                "GS" (geometry shader).
     --annotation <file>         Load an annotation scene instead of a splat model.
     --first <x> <y> <z>         First-person camera position (not recommended for benchmarks:
                                its orientation differs from --agent-eye).

   Simplification (training-free, see modeling/GaussianSimplifier.h):
     --simplify <ratio>          Reduce the splat count in place before rendering, e.g. 0.5 keeps
                                half of the splats and 0.1 keeps 10%. Paged/LOD models (3D Tiles,
                                LCC, ...) print a notice and skip this option.
                                Note: at close range the screen coverage (and therefore the fill
                                rate) is roughly conserved, so a mild ratio like 0.5 mainly saves
                                vertex/sorting work; use 0.25/0.1 or the shader knobs below to cut
                                fill rate.
     --simplify-opacity <mode>   Opacity of merged splats: average (default), union (NanoGS),
                                or max. "union" composites two layers and darkens more.
     --gauss-radius <sigma>      Max screen-space quad radius of a splat, in sigmas (default 3.0).
                                The quad area scales with its square, so 2.5 removes ~27% of the
                                fragments and 2.0 ~53%, at the cost of truncating the faint tail.
     --gauss-alpha-cut <alpha>   Fragment discard threshold (default 0.01). Raising it (e.g. 0.03)
                                reduces blended fragments but can harden the splat edges.

   The two fill-rate knobs live in the splat shader's vec3 uniform "GaussianParameters":
     x = rendering mode (0: splats, 1: points with key '1', back to 0 with key '0')
     y = max quad radius in sigmas (<= 0 means 3.0)
     z = alpha cutoff (<= 0 means 0.01)
   Defaults (0, 3.0, 0.01) are installed by GlobalGaussianSorter when a model is read
   (readerwriter/UtilitiesEx.cpp) and by this test in --custom mode.

   Output and measurement:
     --save <file>               Write the (possibly simplified) scene, then keep rendering.
     --bench <frames>            Render N frames after 10 warm-up frames, print the average FPS
                                and exit. A vsync-free window is created in this mode.
     --agent ...                 Scripted capture: --agent-frames N, --agent-eye X Y Z,
                                --agent-center X Y Z, --agent-size W H, --agent-output PREFIX
                                (writes PNGs and exits). Add --agent-keep to keep running.
     --custom                    Use the test's own state sets and sorter instead of the global
                                gaussian pipeline (needed by --render-mode / --cuda-sort).
     --cuda-sort                 Use the CUDA radix sorter in --custom mode.
     --screen <n>, --show-bbox, --show-collision, --test-color
                                 Display helpers.

   Examples:
     ./osgVerse_Test_3DGS model.ply --bench 120
     ./osgVerse_Test_3DGS model.ply --simplify 0.5 --bench 120
     ./osgVerse_Test_3DGS model.ply --gauss-radius 2.5 --gauss-alpha-cut 0.03 --bench 120
     ./osgVerse_Test_3DGS model.ply --simplify 0.5 --save simplified.ply
     ./osgVerse_Test_3DGS model.ply --agent --agent-frames 5 --agent-eye 0 -120 40 \
         --agent-center 0 0 0 --agent-output /tmp/shot
*/

#include <osg/io_utils>
#include <osg/ValueObject>
#include <osg/Texture2D>
#include <osg/BlendFunc>
#include <osg/BlendEquation>
#include <osg/Depth>
#include <osg/GraphicsContext>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/Timer>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgGA/FirstPersonManipulator>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <iostream>
#include <sstream>

#include <modeling/Math.h>
#include <modeling/AnnotationMaker.h>
#include <modeling/GaussianGeometry.h>
#include <modeling/GaussianSimplifier.h>
#include <pipeline/Pipeline.h>
#include <pipeline/MultiEffectNode.h>
#include <pipeline/ResourceManager.h>
#include <pipeline/Utilities.h>
#include <ui/Utilities.h>
#include <readerwriter/DatabasePager.h>
#include <VerseCommon.h>

#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif

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

class QwertyManipulator : public osgGA::FirstPersonManipulator
{
public:
    QwertyManipulator(const osg::Vec3d& e)
    :   osgGA::FirstPersonManipulator(), _presetEye(e), _animKeyTime(0.0), _animPlayTime(-1.0)
    {
        setAllowThrow(false); setVerticalAxisFixed(true); setWheelMovement(0.05f, true);
        _outputCallback = new osgVerse::ScreenSnapshotCallback(false, 25);
        _path = new osg::AnimationPath; _path->setLoopMode(osg::AnimationPath::NO_LOOPING);
    }

    virtual void home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
    { osgGA::FirstPersonManipulator::home(ea, us); setTransformation(_presetEye, _homeCenter, _homeUp); }

protected:
    virtual bool handleMouseDrag(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
    {
        if (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL) return false;
        else return osgGA::FirstPersonManipulator::handleMouseDrag(ea, us);
    }

    virtual bool handleKeyDown(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
    { osgVerse::KeyboardCacher::instance()->advance(ea); return false; }

    virtual bool handleKeyUp(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
    {
        osg::View* view = dynamic_cast<osg::View*>(&us);
        if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Page_Down ||  // Look from top-view
            ea.getKey() == osgGA::GUIEventAdapter::KEY_Page_Up)      // Look from front-view
        {
            osg::Vec3d eye, center, up, forward, side; float D = 1.0f;
            view->getCamera()->getViewMatrixAsLookAt(eye, center, up, D);
            forward = center - eye; forward.normalize(); side = forward ^ up;
            
            if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Page_Up)
                { eye = center + osg::Z_AXIS * D; up = -osg::Z_AXIS ^ side; }
            else
                { forward = side ^ osg::Z_AXIS; eye = center - forward * D; up = osg::Z_AXIS; }
            setByInverseMatrix(osg::Matrix::lookAt(eye, center, up));
        }

        else if (ea.getKey() == 'r')  // record an animation keyframe
        {
            osg::Matrix worldMatrix = osg::Matrix::inverse(view->getCamera()->getViewMatrix());
            osg::Vec3d pos = worldMatrix.getTrans(); osg::Quat rot = worldMatrix.getRotate();
            _path->insert(_animKeyTime, osg::AnimationPath::ControlPoint(pos, rot));
            _animKeyTime += 1.0f;
        }
        else if (ea.getKey() == 't')  // save current animation path
            { std::ofstream out("animation_path.txt"); _path->write(out); }
        else if (ea.getKey() == 'o')  // clear current animation path
            { _path->clear(); _animKeyTime = 0.0; }
        else if (ea.getKey() == 'p')  // load and play current animation path
        {
            std::ifstream in("animation_path.txt"); _path->clear(); _path->read(in);
            _animKeyTime = 0.0; _animPlayTime = ea.getTime();
            _outputCallback->setFilePrefix("output"); _outputCallback->setCapturing(true);
            view->getCamera()->setFinalDrawCallback(_outputCallback.get());
        }
        osgVerse::KeyboardCacher::instance()->advance(ea); return false;
    }

    virtual bool handleFrame(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
    {
        float m = (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SHIFT)
                ? (_wheelMovement * _modelSize) : 0.05f;
        osgVerse::KeyboardCacher* kb = osgVerse::KeyboardCacher::instance();
        if (kb->isKeyDown(osgGA::GUIEventAdapter::KEY_Up)) moveForward(m);
        if (kb->isKeyDown(osgGA::GUIEventAdapter::KEY_Down)) moveForward(-m);
        if (kb->isKeyDown(osgGA::GUIEventAdapter::KEY_Left)) moveRight(-m);
        if (kb->isKeyDown(osgGA::GUIEventAdapter::KEY_Right)) moveRight(m);

        if (kb->isKeyDown('h')) _eye.x() -= m; else if (kb->isKeyDown('k')) _eye.x() += m;
        if (kb->isKeyDown('j')) _eye.y() -= m; else if (kb->isKeyDown('u')) _eye.y() += m;
        if (kb->isKeyDown('y')) _rotation *= osg::Quat(-0.01, osg::Z_AXIS);
        else if (kb->isKeyDown('i')) _rotation *= osg::Quat(0.01, osg::Z_AXIS);

        if (_animPlayTime > 0.0)
        {   // Animation on path
            double t = ea.getTime() - _animPlayTime, tEnd = _path->getLastTime();
            osg::Matrix matrix; if (_path->getMatrix(t, matrix)) setByMatrix(matrix);

            if (tEnd < t)
            {
                osg::View* view = dynamic_cast<osg::View*>(&us);
                view->getCamera()->setFinalDrawCallback(NULL);
                _outputCallback->setCapturing(false); _animPlayTime = -1.0;
            }
        }
        return false;
    }

    osg::ref_ptr<osgVerse::ScreenSnapshotCallback> _outputCallback;
    osg::ref_ptr<osg::AnimationPath> _path; osg::Vec3d _presetEye;
    double _animKeyTime, _animPlayTime;
};

class GaussianStateVisitor : public osg::NodeVisitor
{
public:
    GaussianStateVisitor(osgVerse::GaussianSorter* s, const std::string& hint, bool testColor)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), _sorter(s), _testColorCustomizing(testColor)
    {
        osg::Shader* vert = osgDB::readShaderFile(osg::Shader::VERTEX, SHADER_DIR + "gaussian_splatting.vert.glsl");
        osg::Shader* geom = osgDB::readShaderFile(osg::Shader::GEOMETRY, SHADER_DIR + "gaussian_splatting.geom.glsl");
        osg::Shader* frag = osgDB::readShaderFile(osg::Shader::FRAGMENT, SHADER_DIR + "gaussian_splatting.frag.glsl");
        if (!vert || !geom || !frag)
        {
            OSG_WARN << "Missing shaders for gaussian splatting." << std::endl;
            return;
        }

        osgVerse::ResourceManager* res = osgVerse::ResourceManager::instance();
        vert->setName("Gaussian_VS"); geom->setName("Gaussian_GS"); frag->setName("Gaussian_FS");
        res->shareShader(vert, true); res->shareShader(geom, true); res->shareShader(frag, true);

        osgVerse::GaussianGeometry::RenderMethod method = osgVerse::GaussianGeometry::INSTANCING;
        if (hint == "TBO") method = osgVerse::GaussianGeometry::INSTANCING_TEXTURE;
        else if (hint == "TEX2D") method = osgVerse::GaussianGeometry::INSTANCING_TEX2D;
        else if (hint == "GS") method = osgVerse::GaussianGeometry::GEOMETRY_SHADER;
        _program = osgVerse::GaussianGeometry::createProgram(vert, (hint == "GS") ? geom : NULL, frag, method);
        _callback = osgVerse::GaussianGeometry::createUniformCallback();

        std::vector<std::string> gsDefinitions; int minGlslVer = 120;
        std::string vsDefinitions; if (_testColorCustomizing) vsDefinitions = "CUSTOMIZED_TEX,";
        if (hint == "TBO") vsDefinitions += std::string("USE_INSTANCING,USE_INSTANCING_TEX");
        else if (hint == "TEX2D") vsDefinitions += std::string("USE_INSTANCING,USE_INSTANCING_TEX2D");
        else if (hint == "GS")
        {
#if defined(OSG_GL3_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
            gsDefinitions.push_back("layout(points) in;");
            gsDefinitions.push_back("layout(triangle_strip, max_vertices = 4) out;");
#endif
        }
        else
        {
            vsDefinitions += std::string("USE_INSTANCING");
            minGlslVer = 430;  // for SSBO compatibility
        }

        // FIXME: it seems import_defines failed in GLCore/GLES mode? Try using ShaderLibrary as fallback
        if (!vsDefinitions.empty()) vert->setUserValue("Definitions", vsDefinitions);

        int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
        glslVer = osg::maximum(glslVer, minGlslVer);
        osgVerse::Pipeline::createShaderDefinitions(vert, cxtVer, glslVer);
        osgVerse::Pipeline::createShaderDefinitions(geom, cxtVer, glslVer, gsDefinitions);
        osgVerse::Pipeline::createShaderDefinitions(frag, cxtVer, glslVer);
    }

    virtual void apply(osg::Geode& node)
    {
        bool hasGaussian = false;
        if (node.getCullCallback() == _callback) { traverse(node); return; }

        for (unsigned int i = 0; i < node.getNumDrawables(); ++i)
        {
            osgVerse::GaussianGeometry* gs = dynamic_cast<osgVerse::GaussianGeometry*>(node.getDrawable(i));
            if (gs && _sorter.valid())
            {   // to sort geometries by depth
                gs->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                _sorter->addGeometry(gs); hasGaussian = true;

                if (_testColorCustomizing)
                {
                    std::vector<osg::Vec4> param(gs->getNumSplats(), osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
#if false
                    osgVerse::PointCloudSegmentation segmentation;
                    segmentation.setPairwiseLinkageFactors(100, osg::PI_2, 1);

                    std::vector<osg::Vec3d> allPoints(gs->getNumSplats());
                    {
                        osg::Vec4* pts4 = gs->getPosition4();
                        if (pts4 != NULL)
                        {
                            for (size_t i = 0; i < allPoints.size(); ++i)
                            { const osg::Vec4& p = pts4[i]; allPoints[i] = osg::Vec3(p[0], p[1], p[2]); }
                        }
                        else
                        {
                            osg::Vec3* pts3 = gs->getPosition3();
                            if (pts3 != NULL) { for (size_t i = 0; i < allPoints.size(); ++i) allPoints[i] = pts3[i]; }
                        }
                    }

                    static osg::Vec3 colors[] = {
                        { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 1.0f, 0.5f, 0.0f }, { 1.0f, 0.0f, 0.5f },
                        { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }, { 0.5f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.5f }, { 0.5f, 1.0f, 0.5f },
                        { 0.0f, 0.0f, 1.0f }, { 0.5f, 0.0f, 1.0f }, { 0.0f, 0.5f, 1.0f }, { 0.5f, 0.5f, 1.0f }, { 0.5f, 0.5f, 0.5f }
                    };

                    std::vector<std::vector<int>> clusters = segmentation.execute(allPoints);
                    for (size_t i = 0; i < clusters.size(); ++i)
                    {
                        const std::vector<int>& cluster = clusters[i];
                        for (size_t j = 0; j < cluster.size(); ++j)
                            param[cluster[j]] = osg::Vec4(colors[i % 15], 1.0f);  // a = 1: replacing; a = 2: additive
                    }
#else
                    for (size_t i = 0; i < param.size() / 2; ++i)
                        param[i] = osg::Vec4(1.0f, 0.0f, 0.0f, 2.0f);  // a = 1: replacing; a = 2: additive
#endif
                    gs->setColorParameters(param);
                }
            }
        }

        if (hasGaussian)
        {
            osg::StateSet* ss = node.getOrCreateStateSet();
            ss->setAttribute(_program.get());
            ss->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
            ss->setAttributeAndModes(new osg::BlendEquation(osg::BlendEquation::FUNC_ADD));
            ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false));
            ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            node.setCullCallback(_callback.get());
        }
        traverse(node);
    }

protected:
    osg::ref_ptr<osg::Program> _program;
    osg::ref_ptr<osg::NodeCallback> _callback;
    osg::observer_ptr<osgVerse::GaussianSorter> _sorter;
    bool _testColorCustomizing;
};

struct CudaRadixSorter : public osgVerse::GaussianSorter::UserCallback
{
    CudaRadixSorter() { _context = osgVerse::CudaAlgorithm::initializeContext(0); }
    ~CudaRadixSorter() { osgVerse::CudaAlgorithm::deinitializeContext(_context); }
    CUcontext _context;

    virtual bool sort(osg::VectorGLuint* indices, osg::Vec3* pos, size_t numSplats,
                      const osg::Matrix& model, const osg::Matrix& view)
    {
        osg::Matrix localToEye = model * view; size_t size = indices->size();
        std::vector<unsigned int> inValues(size), outIDs(size);
        std::vector<unsigned int> inIDs(indices->begin(), indices->end());

        for (size_t i = 0; i < size; ++i)
        {   // comparing floating-point numbers as integers
            float d = (pos[(*indices)[i]] * localToEye).z();
            union { float f; uint32_t u; } un = { (d > 0.0f ? 0.0f : (-d)) };
            inValues[i] = (GLuint)un.u; //if (d > 0.0f) numCulled++;
        }

        if (osgVerse::CudaAlgorithm::radixSort(inValues, inIDs, outIDs))
        {
            indices->assign(outIDs.rbegin(), outIDs.rend());
            return true;
        }
        return false;
    }

    virtual bool sort(osg::VectorGLuint* indices, osg::Vec4* pos, size_t numSplats,
                      const osg::Matrix& model, const osg::Matrix& view)
    {
        osg::Matrix localToEye = model * view; size_t size = indices->size();
        std::vector<unsigned int> inValues(size), outIDs(size);
        std::vector<unsigned int> inIDs(indices->begin(), indices->end());

        for (size_t i = 0; i < size; ++i)
        {   // comparing floating-point numbers as integers
            const osg::Vec4& p = pos[(*indices)[i]];
            float d = (osg::Vec3(p[0], p[1], p[2]) * localToEye).z();
            union { float f; uint32_t u; } un = { (d > 0.0f ? 0.0f : (-d)) };
            inValues[i] = (GLuint)un.u; //if (d > 0.0f) numCulled++;
        }

        if (osgVerse::CudaAlgorithm::radixSort(inValues, inIDs, outIDs))
        {
            indices->assign(outIDs.rbegin(), outIDs.rend());
            return true;
        }
        return false;
    }
};

void interactiveEditAnnotations(osgVerse::AnnotationMaker* maker,
                                const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us)
{
    osg::View* view = dynamic_cast<osg::View*>(&us);
    if (ea.getEventType() == osgGA::GUIEventAdapter::DRAG &&
        (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL))
    {
        std::cout << "DRAG " << ea.getX() << ", " << ea.getY() << "\n";
    }
}

void createAnnotationScene(osg::Group* root, osgVerse::AnnotationMaker* maker)
{
    osg::Geode* geode = maker->getOrCreateGeode();
    osg::Geode* geode2 = maker->getOrCreateTextGeode();
    if (geode && geode2)
    {
        osg::Group* labelRoot = new osg::Group;
        labelRoot->addChild(geode); labelRoot->addChild(geode2);
        labelRoot->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        labelRoot->getOrCreateStateSet()->setTextureAttribute(0, osgVerse::createDefaultTexture());
        labelRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("baseTexture", (int)0));
        root->addChild(labelRoot);
    }
}

// Simplify all Gaussian geometries in the scene and replace them in their geodes.
// The sorter is updated as well when it has already been registered by the reader.
struct GaussianSimplifyVisitor : public osg::NodeVisitor
{
    GaussianSimplifyVisitor(float ratio, int opacityMode, osgVerse::GaussianSorter* sorter)
    :   osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), _ratio(ratio), _opacityMode(opacityMode),
        _sorter(sorter), _numReplaced(0) {}

    virtual void apply(osg::Geode& node)
    {
        for (unsigned int i = 0; i < node.getNumDrawables(); ++i)
        {
            osgVerse::GaussianGeometry* geom =
                dynamic_cast<osgVerse::GaussianGeometry*>(node.getDrawable(i));
            if (!geom) continue;

            osgVerse::GaussianSimplifyOptions opt;
            opt.ratio = _ratio; opt.opacityMode = _opacityMode; opt.verbose = true;
            osg::ref_ptr<osgVerse::GaussianGeometry> simplified =
                osgVerse::GaussianSimplifier::simplifyGeometry(*geom, opt);
            if (!simplified.valid()) continue;

            std::cout << "[Simplify] " << geom->getNumSplats() << " -> "
                      << simplified->getNumSplats() << " splats (SH degree "
                      << simplified->getShDegrees() << ")" << std::endl;
            if (_sorter)
            {
                _sorter->removeGeometry(geom);
                _sorter->addGeometry(simplified.get());
            }
            node.replaceDrawable(geom, simplified.get()); _numReplaced++;
        }
        traverse(node);
    }

    unsigned int getNumReplaced() const { return _numReplaced; }

protected:
    float _ratio;
    int _opacityMode;
    osgVerse::GaussianSorter* _sorter;
    unsigned int _numReplaced;
};

// Paged/LOD scenes (3D Tiles, LCC, ...) can't be simplified as a whole
struct LodNodeFinder : public osg::NodeVisitor
{
    LodNodeFinder() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), found(false) {}

    virtual void apply(osg::PagedLOD& node) { found = true; traverse(node); }
    virtual void apply(osg::LOD& node) { found = true; traverse(node); }
    virtual void apply(osg::ProxyNode& node) { found = true; traverse(node); }
    bool found;
};

// Simple FPS benchmark: skips the first warm-up frames (shader/pipeline compilation) and quits
// after the requested number of frames, so load and simplification time are not counted
struct FrameBenchmarkHandler : public osgGA::GUIEventHandler
{
    FrameBenchmarkHandler(int frames)
    :   _frames(frames), _count(0), _warmup(10) { _timer.setStartTick(); }

    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        if (ea.getEventType() != osgGA::GUIEventAdapter::FRAME) return false;
        ++_count;
        if (_count == _warmup) _timer.setStartTick();
        else if (_count >= _warmup + _frames)
        {
            double t = _timer.time_s();
            std::cout << "[Bench] " << _frames << " frames in " << t << "s = "
                      << (_frames / t) << " FPS" << std::endl;
            osgViewer::ViewerBase* vb = dynamic_cast<osgViewer::ViewerBase*>(&aa);
            if (vb) vb->setDone(true);
        }
        return false;
    }

protected:
    osg::Timer _timer; int _frames, _count, _warmup;
};

// Optional fill-rate knobs of the splat shader (defaults keep the original behaviour).
// They live in the same vec3 uniform as the rendering mode: x=mode, y=radius, z=cutoff.
static void applyGaussianFillOptions(osg::Node* node, float radius, float cutoff)
{
    if (!node || (radius <= 0.0f && cutoff <= 0.0f)) return;
    osg::StateSet* ss = node->getOrCreateStateSet();
    osg::Uniform* param = ss->getUniform("GaussianParameters");
    if (!param)
    {
        param = new osg::Uniform("GaussianParameters", osg::Vec3(0.0f, 3.0f, 0.01f));
        ss->addUniform(param);
    }

    osg::Vec3 value; param->get(value);
    if (radius > 0.0f) value.y() = radius;
    if (cutoff > 0.0f) value.z() = cutoff;
    param->set(value);
}

static void simplifyGaussianNode(osg::Node* node, float ratio, int opacityMode,
                                 osgVerse::GaussianSorter* sorter)
{
    if (!(ratio > 0.0f && ratio < 1.0f))
    {
        std::cout << "[Simplify] Invalid ratio " << ratio << ", option ignored." << std::endl;
        return;
    }

    LodNodeFinder lf; node->accept(lf);
    if (lf.found)
    {
        std::cout << "[Simplify] Paged/LOD model (3D Tiles, LCC, ...) is not supported, "
                  << "option ignored." << std::endl;
        return;
    }

    GaussianSimplifyVisitor sv(ratio, opacityMode, sorter); node->accept(sv);
    if (sv.getNumReplaced() == 0)
        std::cout << "[Simplify] No Gaussian geometry found, option ignored." << std::endl;
}

int main(int argc, char** argv)
{
    osgViewer::Viewer viewer;
    viewer.getCamera()->setClearColor(osg::Vec4(0.1f, 0.1f, 0.15f, 0.0f));
    //viewer.getCamera()->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);  // not necessary
    viewer.getCamera()->setProjectionMatrixAsPerspective(30.0, 16.0 / 9.0, 0.1, 10000.0);
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);

    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
    osgDB::Registry::instance()->addFileExtensionAlias("ply", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("spz", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("splat", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("ksplat", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("lcc", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("lcc2", "verse_3dgs");
    osgDB::Registry::instance()->addFileExtensionAlias("sog", "verse_3dgs");

    osgVerse::HeadUpDisplayCanvas hudCanvas;
    osg::ArgumentParser arguments(&argc, argv);
    std::string optString; arguments.read("-O", optString);
    std::string savedFile; arguments.read("--save", savedFile);
    float simplifyRatio = 0.0f; arguments.read("--simplify", simplifyRatio);
    std::string opacityName; arguments.read("--simplify-opacity", opacityName);
    int opacityMode = 1;  // 0: union (NanoGS), 1: average, 2: max
    if (opacityName == "union") opacityMode = 0;
    else if (opacityName == "max") opacityMode = 2;
    int benchFrames = 0; arguments.read("--bench", benchFrames);
    float gaussRadius = 0.0f; arguments.read("--gauss-radius", gaussRadius);
    float gaussCutoff = 0.0f; arguments.read("--gauss-alpha-cut", gaussCutoff);

    // Agent options must be consumed before reading models, or they are taken as file names.
    // The camera view is applied here and the manipulator is skipped when an eye is given.
    bool hasAgentEye = (arguments.find("--agent-eye") >= 0) ||
                       (arguments.find("--agent-center") >= 0);
    bool agentMode = osgVerse::configureViewerForAgent(arguments, &viewer);

    double firstX = 0.0, firstY = 0.0, firstZ = 0.0;
    bool fpsView = arguments.read("--first", firstX, firstY, firstZ);

    if (!arguments.read("--custom"))
    {
        // Simplest forward rendering implementation
        osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
        osgVerse::updateOsgBinaryWrappers();

        osg::ref_ptr<osg::Node> gs = osgDB::readNodeFiles(arguments, new osgDB::Options(optString));
        if (!gs) gs = osgDB::readNodeFile(BASE_DIR + "/models/3dgs_parrot.splat", new osgDB::Options(optString));
        if (!gs) { std::cout << "No 3DGS file loaded" << std::endl; return 1; }

        // Note: geometries are already registered to the global sorter by the reader
        osgVerse::GaussianSorter* sorter = static_cast<osgVerse::GaussianSorter*>(
            osgVerse::getGlobalFileCallback()->getGaussian()->sorterBase.get());
        simplifyGaussianNode(gs.get(), simplifyRatio, opacityMode, sorter);
        applyGaussianFillOptions(gs.get(), gaussRadius, gaussCutoff);

        gs->setCullCallback(osgVerse::GaussianGeometry::createUniformCallback());
        root->addChild(gs.get()); viewer.setSceneData(root.get());
        viewer.getCamera()->setPreDrawCallback(new osgVerse::GaussianSortCallback(sorter));
    }
    else
    {
        osgVerse::globalInitialize(
            argc, argv, osgVerse::defaultInitParameters(osgVerse::NoParameters));  // disable default sorter for test...
        osgVerse::updateOsgBinaryWrappers();

        std::string annotation; arguments.read("--annotation", annotation);
        std::string hint; arguments.read("--render-mode", hint);
        std::string vOffset; arguments.read("--vertex-offset", vOffset);
        std::string vCount; arguments.read("--vertex-count", vCount);
        bool testColor = arguments.read("--test-color");
        osg::ref_ptr<osgDB::Options> options = new osgDB::Options(
            "RenderMethod=" + hint + " LoadVertexOffset=" + vOffset + " LoadVertexCount=" + vCount + " " + optString);

        osg::ref_ptr<osg::Node> gs = osgDB::readNodeFiles(arguments, options.get());
        if (!gs) gs = osgDB::readNodeFile(BASE_DIR + "/models/3dgs_parrot.splat", options.get());
        if (!gs) { std::cout << "No 3DGS file loaded" << std::endl; return 1; }

        // No sorter is created yet in this mode, so only replace the geometries
        simplifyGaussianNode(gs.get(), simplifyRatio, opacityMode, NULL);

        // x: rendering mode, y: max quad radius, z: alpha cutoff (same defaults as GlobalGaussianSorter)
        root->getOrCreateStateSet()->getOrCreateUniform("GaussianParameters", osg::Uniform::FLOAT_VEC3)
            ->set(osg::Vec3(0.0f, 3.0f, 0.01f));
        applyGaussianFillOptions(root.get(), gaussRadius, gaussCutoff);
        root->addChild(gs.get()); root->addChild(hudCanvas.create(1920, 1080));
        viewer.setSceneData(root.get());

        osgVerse::QuickEventHandler* handler = new osgVerse::QuickEventHandler;
        handler->addKeyUpCallback('1', [&](int key) {
            osg::Uniform* u = root->getStateSet()->getUniform("GaussianParameters");
            osg::Vec3 v; u->get(v); u->set(osg::Vec3(1.0f, v.y(), v.z()));
        });
        handler->addKeyUpCallback('0', [&](int key) {
            osg::Uniform* u = root->getStateSet()->getUniform("GaussianParameters");
            osg::Vec3 v; u->get(v); u->set(osg::Vec3(0.0f, v.y(), v.z()));
        });
        viewer.addEventHandler(handler);

        osg::ref_ptr<osgVerse::GaussianSorter> sorter = new osgVerse::GaussianSorter;
        if (arguments.read("--cuda-sort")) sorter->setSortCallback(new CudaRadixSorter);

        GaussianStateVisitor gsv(sorter.get(), hint, testColor); gs->accept(gsv);
        viewer.getCamera()->setPreDrawCallback(new osgVerse::GaussianSortCallback(sorter.get()));
        
        // for paging gaussian data, we should refresh newly loaded tiles to the sorter
        handler->setHandleCallback([&](const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) -> bool
        {
            static GaussianStateVisitor s_visitor = gsv;
            if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            { s_visitor.reset(); root->accept(s_visitor); }
            return false;
        });

        if (!annotation.empty())
        {   // Load annotation json file
            std::ifstream fin(annotation.c_str());
            osg::ref_ptr<osgVerse::AnnotationMaker> maker = new osgVerse::AnnotationMaker;
            if (maker->load(fin, true)) createAnnotationScene(root.get(), maker);

            handler->setHandleCallback([maker](const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
                { interactiveEditAnnotations(maker.get(), ea, aa); return false; }
            );
            //hudCanvas.createText("main", L"Annotations", 32, 200, 40);
        }
    }

    if (arguments.read("--show-bbox"))
    {
        osgVerse::DatabasePager* pager = new osgVerse::DatabasePager;
        pager->setDrawBoundingBox(true);
        viewer.setDatabasePager(pager);
    }

    int screenNo = 0; arguments.read("--screen", screenNo);
    if (benchFrames > 0)
    {   // Benchmark without vsync, otherwise 60Hz display reports only 60/30 fps buckets
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->x = 0; traits->y = 0; traits->width = 1920; traits->height = 1080;
        traits->windowDecoration = false; traits->doubleBuffer = true;
        traits->vsync = false; traits->screenNum = screenNo;
        osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
        if (gc.valid())
        {
            gc->setClearColor(viewer.getCamera()->getClearColor());
            viewer.getCamera()->setGraphicsContext(gc.get());
            viewer.getCamera()->setViewport(new osg::Viewport(0, 0, 1920, 1080));
        }
        else
            viewer.setUpViewOnSingleScreen(screenNo);
    }
    else
        viewer.setUpViewOnSingleScreen(screenNo);
    if (arguments.read("--show-collision"))
    {
        osgVerse::QuickObjectVisitor qov; qov.setNodeMaskOverride(0xffffffff);  // to visit nodes with mask=0
        qov.setNodeFinder([](osg::Object& node) -> bool
            {
                bool isCollider = false; node.getUserValue("Collision", isCollider);
                if (isCollider) static_cast<osg::Node&>(node).setNodeMask(0xffffffff); return false;
            });
        root->accept(qov);  // find collision node and display it, only work for LCC scene
    }

    if (!agentMode || !hasAgentEye)
    {
        if (fpsView) viewer.setCameraManipulator(new QwertyManipulator(osg::Vec3d(firstX, firstY, firstZ)));
        else viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    }
    if (benchFrames > 0) viewer.addEventHandler(new FrameBenchmarkHandler(benchFrames));
    
    if (!savedFile.empty()) osgDB::writeNodeFile(*root, savedFile);
    return viewer.run();
}
