#include <osg/io_utils>
#include <osg/GL>
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

// perf_profile_test: load one or more models from the command line and report, for the model
// currently selected with the left/right arrow keys, its GPU cost, submitted work and resource
// footprint, together with the problems PerfAnalyzer derives from those numbers.
//
//   osgVerse_Test_Perf_Profile modelA.osgb [modelB.osgb ...] [--overlap] [--no-hud] [--log]
//
// --overlap stacks all models at the same place instead of laying them out in a row, which is
// useful to compare how several assets behave when they are rendered together.
// --no-hud profiles without the canvas (to check the cost of the profiler itself) and --log
// echoes the same report to stdout.

namespace
{
    struct ModelInfo
    {
        std::string file, name;
        osg::observer_ptr<osg::MatrixTransform> root;
    };

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

    class PerfHudHandler : public osgGA::GUIEventHandler
    {
    public:
        PerfHudHandler(osgVerse::HeadUpDisplayCanvas* hud, osgVerse::PerfProfiler* perf,
                       std::vector<ModelInfo>* models, int* selected, bool logText)
            : _hud(hud), _perf(perf), _models(models), _selected(selected), _logText(logText),
              _fps(0.0), _frameMs(0.0), _lastTime(0.0), _lastUpdate(0.0), _firstFrame(true) {}

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN)
            {
                int count = (int)_models->size();
                if (count > 0 && ea.getKey() == osgGA::GUIEventAdapter::KEY_Left)
                    *_selected = (*_selected + count - 1) % count, updateText();
                else if (count > 0 && ea.getKey() == osgGA::GUIEventAdapter::KEY_Right)
                    *_selected = (*_selected + 1) % count, updateText();
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

            std::ostringstream out;
            out << std::fixed << std::setprecision(1)
                << "FPS " << _fps << "   (" << _frameMs << " ms)      model "
                << (*_selected + 1) << "/" << _models->size() << "\n"
                << "------------------------------------------------------------\n";

            ModelInfo& cur = (*_models)[*_selected];
            const osgVerse::PerfProfiler::Result* r = _perf->getResult(cur.name);
            out << cur.name << "\n";
            if (r)
            {
                const osgVerse::PerfProfiler::Sample& s = r->total;
                out << "  gpu      " << std::setprecision(3) << s.gpuMs << " ms\n"
                    << "  draw     " << formatCount(double(s.drawCalls)) << " calls / "
                    << formatCount(double(s.drawables)) << " drawables\n"
                    << "  prim     " << formatCount(double(s.primitives)) << "   vs "
                    << formatCount(double(s.vsInvocations)) << "   fs "
                    << formatCount(double(s.fsInvocations)) << "\n"
                    << "  samples  " << formatCount(double(s.samplesPassed));
                if (s.pixels > 0)
                    out << "   overdraw " << std::setprecision(2)
                        << double(s.fsInvocations) / double(s.pixels) << "x   coverage "
                        << double(s.samplesPassed) / double(s.pixels) << "x";
                out << "\n  geometry " << formatMB(s.geometryBytes) << " MB ("
                    << s.geometries << ")\n"
                    << "  textures " << formatMB(s.textureBytes) << " MB (" << s.textures
                    << ", exclusive " << formatMB(s.texExclusiveBytes) << " MB)\n"
                    << "  buffers  " << formatMB(s.bufferBytes) << " MB\n";
            }
            else
                out << "  (waiting for samples)\n";

            // Problems derived from the raw counters
            out << "------------------------------------------------------------\n"
                << "problems:\n";
            std::vector<osgVerse::PerfAnalyzer::Report> reports =
                osgVerse::PerfAnalyzer::analyze(*_perf);
            const osgVerse::PerfAnalyzer::Report* mine = NULL;
            for (size_t i = 0; i < reports.size(); ++i)
                if (reports[i].name == cur.name) { mine = &reports[i]; break; }
            if (mine && !mine->findings.empty())
            {
                for (size_t i = 0; i < mine->findings.size(); ++i)
                {
                    const osgVerse::PerfAnalyzer::Finding& f = mine->findings[i];
                    out << "  - " << f.code << " (" << std::setprecision(2) << f.score << ") "
                        << f.title << "\n";
                }
            }
            else
                out << "  none (no dominant pressure)\n";

            // Compact table of all models, so several assets can be compared at a glance
            out << "------------------------------------------------------------\n"
                << "all models            gpu(ms)  draws   geom(MB)  tex(MB)\n";
            for (size_t i = 0; i < _models->size(); ++i)
            {
                const osgVerse::PerfProfiler::Result* ri = _perf->getResult((*_models)[i].name);
                std::string n = (*_models)[i].name;
                if (n.size() > 20) n = n.substr(0, 20);
                out << (int(i) == *_selected ? "> " : "  ") << std::left << std::setw(21) << n;
                if (ri)
                {
                    const osgVerse::PerfProfiler::Sample& si = ri->total;
                    out << std::right << std::setw(7) << std::setprecision(2) << si.gpuMs
                        << std::setw(7) << formatCount(double(si.drawCalls))
                        << std::setw(10) << formatMB(si.geometryBytes)
                        << std::setw(9) << formatMB(si.textureBytes) << "\n";
                }
                else out << "      -\n";
            }
            out << "\nleft / right arrow keys switch the profiled model\n";

            if (_logText) std::cout << out.str() << std::flush;
            if (!_hud) return;
            std::map<std::string, osg::ref_ptr<osgText::Text>>::iterator it = _hud->texts.find("perf");
            if (it != _hud->texts.end() && it->second.valid()) it->second->setText(out.str());
        }

    protected:
        osgVerse::HeadUpDisplayCanvas* _hud;
        osgVerse::PerfProfiler* _perf;
        std::vector<ModelInfo>* _models;
        int* _selected;
        bool _logText;
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

    bool overlap = arguments.read("--overlap");
    int screenNo = 0; arguments.read("--screen", screenNo);

    // Positional arguments that really exist are treated as models; option values are skipped
    std::vector<std::string> files;
    for (int i = 1; i < arguments.argc(); ++i)
    {
        const char* arg = arguments[i];
        if (!arg || !arg[0] || arg[0] == '-') continue;
        if (osgDB::fileExists(arg)) files.push_back(arg);
    }
    if (files.empty())
    {
        OSG_WARN << "[perf_profile_test] Usage: osgVerse_Test_Perf_Profile <model1> [model2 ...] "
                 << "[--overlap] [--no-hud] [--log]\n";
        return 1;
    }

    osg::ref_ptr<osg::Group> root = new osg::Group; root->setName("PerfRoot");
    osgVerse::PerfProfiler* perf = osgVerse::PerfProfiler::instance();
    perf->setResourceInterval(120);   // scene traversal for the resource numbers

    std::vector<ModelInfo> models;
    osg::BoundingSphere modelsBound;   // union of the loaded models, used for the default view
    double cursor = 0.0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        osg::ref_ptr<osg::Node> node = osgDB::readNodeFile(files[i]);
        if (!node) { OSG_WARN << "[perf_profile_test] Failed to load " << files[i] << std::endl; continue; }

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
        // The models carry no shaders: give them a simple textured forward program on GL3 / ES
        node->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        node->getOrCreateStateSet()->addUniform(new osg::Uniform("baseTexture", (int)0));
#endif
        osg::BoundingSphere bs = node->getBound();
        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        if (!overlap)
        {
            mt->setMatrix(osg::Matrix::translate(cursor, 0.0, 0.0));
            cursor += std::max(bs.radius() * 2.2, 1.0);
        }
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

    int selected = 0;
    PerfHudHandler* handler = new PerfHudHandler(&hud, perf, &models, &selected, logText);
    viewer.addEventHandler(handler);
    perf->watchCamera(viewer.getCamera(), "MainCamera", osgVerse::PerfProfiler::COUNTERS);

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
