#include <osg/io_utils>
#include <osg/GL>
#include <osg/Depth>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgGA/GUIEventHandler>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgText/Text>

#include <pipeline/Global.h>
#include <pipeline/NodeSelector.h>
#include <pipeline/PerfProfiler.h>
#include <pipeline/PerfAnalyzer.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <ui/Utilities.h>
#include <wrappers/Export.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
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

// perf_profile_test: load one or more models (or every model inside one or more directories) and
// report, for the model currently selected with the keyboard, its GPU cost, submitted work and
// resource footprint, together with the problems PerfAnalyzer derives from those numbers.
//
//   osgVerse_Test_Perf_Profile <model|directory> [model|directory ...]
//                              [--max N] [--no-hud] [--no-perf] [--log]
//
// The models keep their own positions in the scene: nothing is moved or normalized, they are only
// wrapped by an identity transform for naming and profiling. --max limits how many models are
// loaded from the directories (0 = all).
// The selected model is highlighted with a yellow bounding box (osgVerse::NodeSelector).
// --no-hud profiles without the canvas (to check the cost of the profiler itself), --no-perf
// turns the collection off completely (to check the whole tool against a plain viewer) and --log
// echoes the same report to stdout.
// The HUD only shows a window of the model list: left/right/up/down switch the selected model,
// PgUp/PgDn jump 10 entries and Home/End select the first/last one.

namespace
{
    struct ModelInfo
    {
        std::string file, name;
        osg::observer_ptr<osg::MatrixTransform> root;
    };

    enum { MAX_LIST_ROWS = 8 };   // rows of the model list shown on the HUD

    std::string formatCount(double v)
    {
        char buf[64];
        if (v >= 1e9) sprintf(buf, "%.2fG", v / 1e9);
        else if (v >= 1e6) sprintf(buf, "%.2fM", v / 1e6);
        else if (v >= 1e3) sprintf(buf, "%.2fk", v / 1e3);
        else sprintf(buf, "%.0f", v);
        return std::string(buf);
    }

    std::string formatMB(unsigned long long bytes)
    {
        char buf[64]; sprintf(buf, "%.1f", double(bytes) / (1024.0 * 1024.0)); return std::string(buf);
    }

    // Formats supported by the OSG / osgVerse readers: a directory argument is expanded to all
    // its first-level entries with one of these extensions (sub-directories are not visited)
    bool isModelFile(const std::string& file)
    {
        static const char* s_extensions[] = { "osgb", "osg", "osgt", "osgx", "ive", "gltf", "glb",
                                              "b3dm", "fbx", "obj", "ply", "dae", "stl",
                                              "verse_gltf", "verse_tiles", "verse_3dgs" };
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        for (size_t i = 0; i < sizeof(s_extensions) / sizeof(s_extensions[0]); ++i)
            if (ext == s_extensions[i]) return true;
        return false;
    }

    void collectModels(const std::string& path, std::vector<std::string>& out)
    {
        if (!osgDB::fileExists(path)) return;
        if (osgDB::fileType(path) != osgDB::DIRECTORY)
        {
            if (isModelFile(path)) out.push_back(path);
            return;
        }

        osgDB::DirectoryContents files = osgDB::getSortedDirectoryContents(path);
        for (size_t i = 0; i < files.size(); ++i)
        {
            if (files[i] == "." || files[i] == "..") continue;
            std::string full = osgDB::concatPaths(path, files[i]);
            if (osgDB::fileType(full) != osgDB::REGULAR_FILE) continue;
            if (isModelFile(full)) out.push_back(full);
        }
    }

    std::string shortenName(const std::string& name, size_t maxChars = 22)
    {
        if (name.size() <= maxChars) return name;
        return name.substr(0, maxChars - 3) + "...";
    }

    // Label of a PerfAnalyzer finding shown on the HUD.
    // Each label states WHAT was measured, so that the advice below it makes sense on its own
    std::string findingLabel(const std::string& code)
    {
        if (code == "GEOMETRY_PRESSURE")
            return "Geometry/vertex pressure: many primitives and vertices, "
                   "time goes to vertex processing";
        if (code == "VERTEX_DUPLICATION")
            return "Vertex duplication: vertices are not shared through indices, "
                   "vertex count far exceeds primitives";
        if (code == "TINY_TRIANGLES")
            return "Tiny triangles: many triangles cover very few pixels";
        if (code == "FRAGMENT_PRESSURE")
            return "Fragment pressure: fragment shading and texture sampling dominate";
        if (code == "OVERDRAW")
            return "High overdraw: the same pixel is shaded repeatedly";
        if (code == "WASTED_SHADING")
            return "Wasted fragments: shading ran but the depth test discarded them";
        if (code == "FILL_RATE")
            return "Fill-rate bound: depth test, blending and framebuffer writes are expensive";
        if (code == "FIXED_COST")
            return "Fixed cost dominates: little is drawn but GPU time is still consumed";
        if (code == "DRAW_OVERHEAD")
            return "High per-draw overhead: state changes and submission dominate";
        if (code == "BATCHING")
            return "Poor batching: too few primitives per object, too many draw calls";
        if (code == "SPIKE")
            return "Time spike: this frame is clearly slower than the recent ones";
        if (code == "RESOURCE_PRESSURE")
            return "High resource usage: large geometry or texture memory footprint";
        if (code == "FULLY_OCCLUDED")
            return "Fully occluded: fragments ran but none passed the depth test";
        if (code == "VRAM_PRESSURE")
            return "Out of VRAM: resources are paged in and out every frame and "
                   "the frame rate collapses";
        if (code == "UNACCOUNTED_TIME")
            return "Unaccounted frame time: not in these render passes; "
                   "paging/upload/sync is slowing it down";
        if (code == "GPU_OUTLIER")
            return "Outlier object: it costs far more than similar ones; "
                   "its resources may be re-uploaded";
        return code;
    }

    // What can be done about it, printed under the label
    std::string findingAdvice(const std::string& code)
    {
        if (code == "GEOMETRY_PRESSURE")
            return "Reduce vertex count (decimation/LOD), merge static geometry, "
                   "and check that all vertices are really visible";
        if (code == "VERTEX_DUPLICATION")
            return "Weld coincident vertices on export and use indexed triangles "
                   "instead of unrolling per triangle";
        if (code == "TINY_TRIANGLES")
            return "Use LOD/decimation for distant objects or merge tiny triangles; "
                   "check for over-tessellation";
        if (code == "FRAGMENT_PRESSURE")
            return "Sample textures less and at lower resolution, simplify the fragment "
                   "shader, reduce render resolution if needed";
        if (code == "OVERDRAW")
            return "Draw opaque objects front-to-back, cull fully occluded objects, "
                   "reduce transparency and particle layers";
        if (code == "WASTED_SHADING")
            return "Draw large occluders first (pre-z) or sort by depth, so occluded "
                   "fragments do not run the shader first";
        if (code == "FILL_RATE")
            return "Reduce full-screen layers and large transparent areas, "
                   "lower resolution or MSAA sample count";
        if (code == "FIXED_COST")
            return "Check for per-frame uploads/compiles/sync; merge state changes "
                   "and reduce small batch submissions";
        if (code == "DRAW_OVERHEAD")
            return "Sort by material and batch, reducing state changes and uniform updates";
        if (code == "BATCHING")
            return "Merge adjacent meshes with the same material or use instancing "
                   "(osg::GeometryInstancing) to cut draw calls";
        if (code == "SPIKE")
            return "Usually a one-off resource upload or shader compile; "
                   "sample several times and pre-warm if needed";
        if (code == "RESOURCE_PRESSURE")
            return "Convert textures to KTX2/BC7 or lower their resolution, merge "
                   "duplicate geometry, avoid multiple copies of the same resource";
        if (code == "FULLY_OCCLUDED")
            return "Such objects can be culled directly (occlusion queries/preprocessing) "
                   "or moved to a distant LOD level";
        if (code == "VRAM_PRESSURE")
            return "Load fewer models at once (--max N), compress or downscale textures, "
                   "disable shadows or lower shadow resolution";
        if (code == "UNACCOUNTED_TIME")
            return "First check whether VRAM is full (then it is paging); otherwise "
                   "check per-frame uploads, shader compiles and driver sync";
        if (code == "GPU_OUTLIER")
            return "Check whether the object's textures/geometry are modified (dirty) "
                   "every frame causing re-uploads, or lower its precision separately";
        return "Confirm the bottleneck direction with the counters above";
    }

    // One finding = one conclusion line plus one "what to do" line. The raw numbers are already
    // visible in the counters above, so they are not repeated here
    void appendFinding(std::ostringstream& out, const char* marker,
                       const osgVerse::PerfAnalyzer::Finding& f)
    {
        out << marker << findingLabel(f.code) << " [ " << f.code << " "
            << std::setprecision(2) << f.score << " ]\n"
            << "    Advice: " << findingAdvice(f.code) << "\n";
    }

    class PerfHudHandler : public osgGA::GUIEventHandler
    {
    public:
        PerfHudHandler(osgVerse::HeadUpDisplayCanvas* hud, osgVerse::PerfProfiler* perf,
                       std::vector<ModelInfo>* models, int* selected, bool logText,
                       osgVerse::NodeSelector* selector)
            : _hud(hud), _perf(perf), _models(models), _selected(selected), _logText(logText),
              _selector(selector), _lastSelected(-1), _fps(0.0), _frameMs(0.0), _lastTime(0.0),
              _lastUpdate(0.0), _firstFrame(true) {}

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN)
            {
                int count = (int)_models->size();
                if (count < 1) return false;

                int step = 0, target = *_selected;
                switch (ea.getKey())
                {
                case osgGA::GUIEventAdapter::KEY_Left:
                case osgGA::GUIEventAdapter::KEY_Up: step = -1; break;
                case osgGA::GUIEventAdapter::KEY_Right:
                case osgGA::GUIEventAdapter::KEY_Down: step = 1; break;
                case osgGA::GUIEventAdapter::KEY_Page_Up: step = -10; break;
                case osgGA::GUIEventAdapter::KEY_Page_Down: step = 10; break;
                case osgGA::GUIEventAdapter::KEY_Home: target = 0; break;
                case osgGA::GUIEventAdapter::KEY_End: target = count - 1; break;
                default: return false;
                }
                if (step != 0) target = (*_selected + step % count + count) % count;
                target = std::max(0, std::min(target, count - 1));
                if (target != *_selected) { *_selected = target; updateText(); }
                return true;
            }
            else if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                double now = osg::Timer::instance()->time_s();
                if (_firstFrame) { _lastTime = now; _firstFrame = false; return false; }

                double dt = now - _lastTime; _lastTime = now;
                if (dt > 1e-4)
                {
                    double fps = 1.0 / dt;
                    if (_fps <= 0.0) _fps = fps;
                    else _fps = _fps * 0.9 + fps * 0.1;
                }
                _frameMs = _fps > 1e-3 ? 1000.0 / _fps : 0.0;
                if (now - _lastUpdate > 0.25) { updateText(); _lastUpdate = now; }
            }
            return false;
        }

        void updateText()
        {
            if (_models->empty()) return;
            const int count = (int)_models->size();
            const int selected = std::max(0, std::min(*_selected, count - 1));
            updateSelection(selected);

            std::ostringstream out;
            out << std::fixed << std::setprecision(1)
                << "FPS " << _fps << " (" << _frameMs << " ms)     Model "
                << (selected + 1) << "/" << count << "\n"
                << "------------------------------------------------------------\n";

            const ModelInfo& cur = (*_models)[selected];
            const osgVerse::PerfProfiler::Result* r = _perf->getResult(cur.name);
            out << shortenName(cur.name, 40) << "\n";
            if (r)
            {
                const osgVerse::PerfProfiler::Sample& s = r->total;
                out << "  GPU      " << std::setprecision(3) << s.gpuMs << " ms\n"
                    << "  Draw calls " << formatCount(double(s.drawCalls)) << " / "
                    << formatCount(double(s.drawables)) << " objects\n"
                    << "  Primitives " << formatCount(double(s.primitives)) << "   Vertices "
                    << formatCount(double(s.vsInvocations)) << "   Fragments "
                    << formatCount(double(s.fsInvocations)) << "\n"
                    << "  Samples passed " << formatCount(double(s.samplesPassed));
                if (s.pixels > 0)
                    out << "   Overdraw " << std::setprecision(2)
                        << double(s.fsInvocations) / double(s.pixels) << "x   Coverage "
                        << double(s.samplesPassed) / double(s.pixels) << "x";
                out << "\n  Geometry   " << formatMB(s.geometryBytes) << " MB ("
                    << s.geometries << ")\n"
                    << "  Textures   " << formatMB(s.textureBytes) << " MB (" << s.textures
                    << ", exclusive " << formatMB(s.texExclusiveBytes) << " MB";
                if (s.texturesUnknown > 0)
                    out << ", " << s.texturesUnknown << " without CPU data";
                out << ")\n"
                    << "  Buffers    " << formatMB(s.bufferBytes) << " MB\n";
            }
            else
                out << "  (waiting for samples)\n";

            // Whole-frame view: total watched resources, driver-reported GPU memory and the problems
            // which only appear when everything is rendered together
            osgVerse::PerfAnalyzer::GlobalReport global =
                osgVerse::PerfAnalyzer::analyzeGlobal(*_perf, _frameMs);
            out << "------------------------------------------------------------\n"
                << "Resources  Geometry " << formatMB(global.geometryBytes) << " MB   Textures "
                << formatMB(global.textureBytes) << " MB   Object GPU total "
                << std::setprecision(2) << global.totalGpuMs << " ms\n";
            if (global.gpuMemory.supported)
                out << "VRAM       " << formatMB(global.gpuMemory.getUsedBytes()) << " / "
                    << formatMB(global.gpuMemory.totalBytes) << " MB (available "
                    << formatMB(global.gpuMemory.availableBytes) << " MB)\n";

            // Problems derived from the raw counters
            out << "------------------------------------------------------------\n"
                << "Possible problems:\n";
            for (size_t i = 0; i < global.findings.size(); ++i)
                appendFinding(out, "  * ", global.findings[i]);
            std::vector<osgVerse::PerfAnalyzer::Report> reports =
                osgVerse::PerfAnalyzer::analyze(*_perf);
            const osgVerse::PerfAnalyzer::Report* mine = NULL;
            for (size_t i = 0; i < reports.size(); ++i)
                if (reports[i].name == cur.name) { mine = &reports[i]; break; }
            if (mine && !mine->findings.empty())
            {
                for (size_t i = 0; i < mine->findings.size(); ++i)
                    appendFinding(out, "  - ", mine->findings[i]);
            }
            else
                out << "  - No clear pressure on this model (all metrics are low)\n";

            // Only a window of the model list is shown: a directory may hold hundreds of files
            out << "------------------------------------------------------------\n";
            int rows = std::min(count, (int)MAX_LIST_ROWS), first = 0;
            if (count > rows)
            {
                first = selected - rows / 2;
                first = std::max(0, std::min(first, count - rows));
            }
            out << "All models (" << (first + 1) << "-" << (first + rows) << " / "
                << count << ")\n";
            for (int i = first; i < first + rows; ++i)
            {
                const osgVerse::PerfProfiler::Result* ri = _perf->getResult((*_models)[i].name);
                out << (i == selected ? "> " : "  ") << shortenName((*_models)[i].name);
                if (ri)
                {
                    const osgVerse::PerfProfiler::Sample& si = ri->total;
                    out << "  " << std::setprecision(2) << si.gpuMs << "ms  "
                        << formatCount(double(si.drawCalls)) << " calls  "
                        << formatMB(si.geometryBytes) << "MB";
                }
                else out << "  -";
                out << "\n";
            }
            out << "\nLeft/Right/Up/Down switch models, PgUp/PgDn skip 10, "
                   "Home/End go first/last\n";

            if (_logText) std::cout << out.str() << std::flush;
            if (!_hud) return;
            std::map<std::string, osg::ref_ptr<osgText::Text>>::iterator it = _hud->texts.find("perf");
            if (it != _hud->texts.end() && it->second.valid())
                it->second->setText(out.str(), osgText::String::ENCODING_UTF8);
        }

    protected:
        // Highlight the profiled model with the yellow bound box of the NodeSelector
        void updateSelection(int selected)
        {
            if (!_selector || selected == _lastSelected) return;
            _lastSelected = selected;
            _selector->clearAllSelectedNodes();
            const osg::observer_ptr<osg::MatrixTransform>& root = (*_models)[selected].root;
            if (root.valid()) _selector->addSelectedNode(root.get());
        }

        osgVerse::HeadUpDisplayCanvas* _hud;
        osgVerse::PerfProfiler* _perf;
        std::vector<ModelInfo>* _models;
        int* _selected;
        bool _logText;
        osgVerse::NodeSelector* _selector;
        int _lastSelected;
        double _fps, _frameMs, _lastTime, _lastUpdate;
        bool _firstFrame;
    };

    osg::Camera* createHud(osgVerse::HeadUpDisplayCanvas& hud, int width, int height)
    {
        osg::Camera* camera = hud.create(width, height);
        int fontSize = std::max(height / 45, 12);  // keep the text readable on any resolution
        hud.createText("perf", L"", fontSize, width - 40, height - 40, "root",
                       osgVerse::HeadUpDisplayCanvas::FREE,
                       osgVerse::HeadUpDisplayCanvas::LEFT | osgVerse::HeadUpDisplayCanvas::TOP);
        osgText::Text* text = hud.texts["perf"].get();
        if (text)
        {
            text->setAlignment(osgText::Text::LEFT_TOP);
            text->setPosition(osg::Vec3(20.0f, (float)height - 20.0f, 0.0f));
            text->setColor(osg::Vec4(0.95f, 0.97f, 1.0f, 1.0f));
            text->setBackdropType(osgText::Text::OUTLINE);
            text->setBackdropColor(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            text->setBackdropOffset(0.06f);
            text->setDataVariance(osg::Object::DYNAMIC);
        }
        return camera;
    }

    // The HUD canvas renders in pixel coordinates, so it has to know the size of the window that
    // will be presented: --agent-size overrides the window size for scripted captures, otherwise
    // setUpViewOnSingleScreen() opens a full-screen window
    void queryHudSize(osg::ArgumentParser& arguments, int& width, int& height)
    {
        width = 0; height = 0;
        for (int i = 0; i + 2 < arguments.argc(); ++i)
        {
            std::string arg = arguments[i] ? arguments[i] : "";
            if (arg == "--agent-size")
            {
                width = atoi(arguments[i + 1]); height = atoi(arguments[i + 2]); break;
            }
        }

        if (width <= 0 || height <= 0)
        {
            unsigned int screenW = 0, screenH = 0;
            osgVerse::obtainScreenResolution(screenW, screenH);
            width = (int)screenW; height = (int)screenH;
        }
        if (width <= 0 || height <= 0) { width = 1920; height = 1080; }
    }
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler);
    osgVerse::updateOsgBinaryWrappers();

    int screenNo = 0; arguments.read("--screen", screenNo);
    int maxFiles = 0; arguments.read("--max", maxFiles);

    // Positional arguments are models, or directories expanded to all the models inside them;
    // option values that are no existing path are simply ignored
    std::vector<std::string> files;
    for (int i = 1; i < arguments.argc(); ++i)
    {
        const char* arg = arguments[i];
        if (!arg || !arg[0] || arg[0] == '-') continue;
        collectModels(arg, files);
    }
    if (maxFiles > 0 && (int)files.size() > maxFiles) files.resize(maxFiles);
    if (files.empty())
    {
        OSG_WARN << "[perf_profile_test] Usage: osgVerse_Test_Perf_Profile <model|directory> "
                 << "[model|directory ...] [--max N] [--no-hud] [--no-perf] [--log]\n";
        return 1;
    }

    osg::ref_ptr<osg::Group> root = new osg::Group; root->setName("PerfRoot");
    osgVerse::PerfProfiler* perf = osgVerse::PerfProfiler::instance();
    perf->setResourceInterval(120);   // scene traversal for the resource numbers

    std::vector<ModelInfo> models;
    osg::BoundingSphere modelsBound;   // union of the loaded models, used for the default view
    for (size_t i = 0; i < files.size(); ++i)
    {
        osg::ref_ptr<osg::Node> node = osgDB::readNodeFile(files[i]);
        if (!node) { OSG_WARN << "[perf_profile_test] Failed to load " << files[i] << std::endl; continue; }
        else { OSG_NOTICE << "File " << (i + 1) << ": " << files[i] << std::endl; }

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
        // The models carry no shaders: give them a simple textured forward program on GL3 / ES
        node->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        node->getOrCreateStateSet()->addUniform(new osg::Uniform("baseTexture", (int)0));
#endif
        // The models are loaded exactly where they are: the transform keeps only a stable name
        // and a node to watch, it never moves the geometry
        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->addChild(node.get());

        std::string name = osgDB::getStrippedName(files[i]);
        mt->setName(name); root->addChild(mt.get());
        modelsBound.expandBy(mt->getBound());

        ModelInfo info; info.file = files[i]; info.name = name; info.root = mt.get();
        models.push_back(info);
        if (!perf->watchNode(mt.get(), name, osgVerse::PerfProfiler::ALL, root.get()))
            OSG_WARN << "[perf_profile_test] Can't watch " << name << std::endl;
    }
    if (models.empty()) { OSG_WARN << "[perf_profile_test] No model loaded" << std::endl; return 1; }

    osgViewer::Viewer viewer;
    viewer.setSceneData(root.get());
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
    // Compatibility profile: the assets rely on fixed-function lighting, which is off by default
    viewer.setLightingMode(osg::View::HEADLIGHT);
    root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::ON);
    root->getOrCreateStateSet()->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
#endif
    viewer.setUpViewOnSingleScreen(screenNo);
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);

    osgVerse::HeadUpDisplayCanvas hud;
    bool showHud = !arguments.read("--no-hud"), logText = arguments.read("--log");
    if (showHud)
    {
        int hudWidth = 0, hudHeight = 0;
        queryHudSize(arguments, hudWidth, hudHeight);
        root->addChild(createHud(hud, hudWidth, hudHeight));
    }

    // Yellow bounding box around the model being profiled. It is drawn after the scene with the
    // depth test disabled, so it stays visible even when the model is big or fully occluding
    osg::ref_ptr<osgVerse::NodeSelector> selector = new osgVerse::NodeSelector;
    selector->setMainCamera(viewer.getCamera());
    selector->setBoundColor(osg::Vec4(1.0f, 1.0f, 0.0f, 1.0f));
    osg::Group* selectorRoot = selector->getAuxiliaryRoot();
    osg::StateSet* selectorSs = selectorRoot->getOrCreateStateSet();
    selectorSs->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0.0, 1.0, false));
    selectorSs->setRenderBinDetails(1000000, "RenderBin");  // after every profiled bin (~1000+)
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
    selectorSs->setAttribute(osgVerse::createDefaultProgram(""));  // vertex color on GL3 / ES
#endif
    root->addChild(selectorRoot);

    int selected = 0;
    PerfHudHandler* handler = new PerfHudHandler(&hud, perf, &models, &selected, logText, selector.get());
    viewer.addEventHandler(handler);
    perf->watchCamera(viewer.getCamera(), "MainCamera", osgVerse::PerfProfiler::COUNTERS);
    if (arguments.read("--no-perf")) perf->setEnabled(false);   // A/B: the cost of the profiler

    // Optional automation for scripted testing: render N frames, capture and quit
    osgVerse::configureViewerForAgent(arguments, &viewer);

    // Frame all the models once the window (and therefore the aspect ratio) is known. Looking from
    // +Z is the useful direction for both the building models and the flat floor-plan datasets
    viewer.realize();
    osgGA::CameraManipulator* manipulator = viewer.getCameraManipulator();
    if (manipulator)
    {
        osg::BoundingSphere bs = modelsBound;
        manipulator->setHomePosition(bs.center() + osg::Vec3(0.0f, 0.0f, bs.radius() * 2.2f),
                                     bs.center(), osg::Vec3(0.0f, 1.0f, 0.0f));
        manipulator->home(0.0);
    }

    handler->updateText();
    return viewer.run();
}
