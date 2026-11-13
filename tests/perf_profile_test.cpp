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

    // Chinese label of a PerfAnalyzer finding, so that the HUD reads naturally for local users.
    // Each label states WHAT was measured, so that the advice below it makes sense on its own
    std::string findingLabel(const std::string& code)
    {
        if (code == "GEOMETRY_PRESSURE") return "几何/顶点压力：图元和顶点很多，耗时用于顶点处理";
        if (code == "VERTEX_DUPLICATION") return "顶点重复：顶点没有共用索引，顶点数远多于图元";
        if (code == "TINY_TRIANGLES") return "三角形过小：大量三角形只覆盖很少像素";
        if (code == "FRAGMENT_PRESSURE") return "片元压力：片元着色和纹理采样占了主要时间";
        if (code == "OVERDRAW") return "Overdraw 过高：同一像素被反复着色";
        if (code == "WASTED_SHADING") return "无效片元：着色完成却被深度测试丢弃";
        if (code == "FILL_RATE") return "填充率受限：深度测试、混合和写帧缓冲开销大";
        if (code == "FIXED_COST") return "固定开销为主：绘制量很小但 GPU 时间照样被消耗";
        if (code == "DRAW_OVERHEAD") return "每次绘制开销偏高：状态切换和提交占主要时间";
        if (code == "BATCHING") return "批处理不足：单个对象图元太少，draw call 偏多";
        if (code == "SPIKE") return "耗时突增：本帧比最近几帧明显变慢";
        if (code == "RESOURCE_PRESSURE") return "资源占用偏高：几何或纹理显存占用大";
        if (code == "FULLY_OCCLUDED") return "完全被遮挡：片元都执行了但没有通过深度测试";
        if (code == "VRAM_PRESSURE") return "显存不足：资源被逐帧换入换出，帧率会突然坍塌";
        if (code == "UNACCOUNTED_TIME") return "帧时间不在这几个渲染通道里：换页/上传/同步拖慢了";
        if (code == "GPU_OUTLIER") return "个别对象耗时远超同类：它的资源可能在反复上传";
        return code;
    }

    // What can be done about it, printed under the label
    std::string findingAdvice(const std::string& code)
    {
        if (code == "GEOMETRY_PRESSURE")
            return "降低顶点数（减面/LOD）、合并静态几何，并检查是否所有顶点都真的可见";
        if (code == "VERTEX_DUPLICATION")
            return "导出时合并重合顶点（weld）并使用索引三角形，避免按三角形展开";
        if (code == "TINY_TRIANGLES")
            return "为远处对象做 LOD/减面，或合并细碎三角形；检查模型是否被过度细分";
        if (code == "FRAGMENT_PRESSURE")
            return "减少纹理采样次数与分辨率、简化片元着色器，必要时降低渲染分辨率";
        if (code == "OVERDRAW")
            return "不透明物从前向后绘制、剔除被完全遮挡的对象，减少半透明与粒子叠层";
        if (code == "WASTED_SHADING")
            return "先画大的遮挡物（pre-z）或按深度排序，避免被遮挡的片元先跑一遍着色器";
        if (code == "FILL_RATE")
            return "减少全屏叠层和大面积半透明，降低分辨率或 MSAA 采样数";
        if (code == "FIXED_COST")
            return "确认是否有每帧上传/编译/同步；合并状态切换，减少小批次提交";
        if (code == "DRAW_OVERHEAD")
            return "按材质排序并合批，减少状态切换和 uniform 更新次数";
        if (code == "BATCHING")
            return "合并相邻同材质网格或改用实例化绘制（osg::GeometryInstancing），减少 draw call";
        if (code == "SPIKE")
            return "多半是一次性资源上传或着色器编译；多次采样确认，必要时提前预热";
        if (code == "RESOURCE_PRESSURE")
            return "把纹理转成 KTX2/BC7 或降低分辨率，合并重复几何，避免同一资源多份拷贝";
        if (code == "FULLY_OCCLUDED")
            return "这类对象可直接剔除（遮挡查询/预处理），或放进远处 LOD 级别";
        if (code == "VRAM_PRESSURE")
            return "减少同时载入的模型（--max N）、压缩或降分辨率纹理、关闭阴影/降低阴影分辨率";
        if (code == "UNACCOUNTED_TIME")
            return "先看显存是否已满（满了就是换页）；否则检查每帧上传、着色器编译和驱动同步";
        if (code == "GPU_OUTLIER")
            return "检查该对象的纹理/几何是否每帧被修改（dirty）导致重复上传，或单独降低其精度";
        return "结合上面的计数确认瓶颈方向";
    }

    // One finding = one conclusion line plus one "what to do" line. The raw numbers are already
    // visible in the counters above, so they are not repeated here
    void appendFinding(std::ostringstream& out, const char* marker,
                       const osgVerse::PerfAnalyzer::Finding& f)
    {
        out << marker << findingLabel(f.code) << " [ " << f.code << " "
            << std::setprecision(2) << f.score << " ]\n"
            << "    建议：" << findingAdvice(f.code) << "\n";
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
                << "帧率 " << _fps << " FPS (" << _frameMs << " ms)     模型 "
                << (selected + 1) << "/" << count << "\n"
                << "------------------------------------------------------------\n";

            const ModelInfo& cur = (*_models)[selected];
            const osgVerse::PerfProfiler::Result* r = _perf->getResult(cur.name);
            out << shortenName(cur.name, 40) << "\n";
            if (r)
            {
                const osgVerse::PerfProfiler::Sample& s = r->total;
                out << "  GPU      " << std::setprecision(3) << s.gpuMs << " ms\n"
                    << "  绘制调用 " << formatCount(double(s.drawCalls)) << " 次 / "
                    << formatCount(double(s.drawables)) << " 个对象\n"
                    << "  图元     " << formatCount(double(s.primitives)) << "   顶点 "
                    << formatCount(double(s.vsInvocations)) << "   片元 "
                    << formatCount(double(s.fsInvocations)) << "\n"
                    << "  通过样本 " << formatCount(double(s.samplesPassed));
                if (s.pixels > 0)
                    out << "   Overdraw " << std::setprecision(2)
                        << double(s.fsInvocations) / double(s.pixels) << "x   覆盖 "
                        << double(s.samplesPassed) / double(s.pixels) << "x";
                out << "\n  几何     " << formatMB(s.geometryBytes) << " MB ("
                    << s.geometries << " 个)\n"
                    << "  纹理     " << formatMB(s.textureBytes) << " MB (" << s.textures
                    << " 个，独占 " << formatMB(s.texExclusiveBytes) << " MB";
                if (s.texturesUnknown > 0)
                    out << "，其中 " << s.texturesUnknown << " 个无 CPU 数据";
                out << ")\n"
                    << "  缓冲     " << formatMB(s.bufferBytes) << " MB\n";
            }
            else
                out << "  （等待采样数据）\n";

            // Whole-frame view: total watched resources, driver-reported GPU memory and the problems
            // which only appear when everything is rendered together
            osgVerse::PerfAnalyzer::GlobalReport global =
                osgVerse::PerfAnalyzer::analyzeGlobal(*_perf, _frameMs);
            out << "------------------------------------------------------------\n"
                << "资源合计 几何 " << formatMB(global.geometryBytes) << " MB   纹理 "
                << formatMB(global.textureBytes) << " MB   对象 GPU 合计 "
                << std::setprecision(2) << global.totalGpuMs << " ms\n";
            if (global.gpuMemory.supported)
                out << "显存     " << formatMB(global.gpuMemory.getUsedBytes()) << " / "
                    << formatMB(global.gpuMemory.totalBytes) << " MB (可用 "
                    << formatMB(global.gpuMemory.availableBytes) << " MB)\n";

            // Problems derived from the raw counters
            out << "------------------------------------------------------------\n"
                << "可能的问题：\n";
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
                out << "  - 该模型无明显压力（各项指标均不高）\n";

            // Only a window of the model list is shown: a directory may hold hundreds of files
            out << "------------------------------------------------------------\n";
            int rows = std::min(count, (int)MAX_LIST_ROWS), first = 0;
            if (count > rows)
            {
                first = selected - rows / 2;
                first = std::max(0, std::min(first, count - rows));
            }
            out << "全部模型 (" << (first + 1) << "-" << (first + rows) << " / " << count << ")\n";
            for (int i = first; i < first + rows; ++i)
            {
                const osgVerse::PerfProfiler::Result* ri = _perf->getResult((*_models)[i].name);
                out << (i == selected ? "> " : "  ") << shortenName((*_models)[i].name);
                if (ri)
                {
                    const osgVerse::PerfProfiler::Sample& si = ri->total;
                    out << "  " << std::setprecision(2) << si.gpuMs << "ms  "
                        << formatCount(double(si.drawCalls)) << "次  "
                        << formatMB(si.geometryBytes) << "MB";
                }
                else out << "  -";
                out << "\n";
            }
            out << "\n左右/上下键切换模型，PgUp/PgDn 跳 10 个，Home/End 首尾\n";

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
