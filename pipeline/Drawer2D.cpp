#include <OpenThreads/ScopedLock>
#include <osg/Version>
#include <osg/io_utils>
#include <osg/ImageUtils>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#define TVG_STATIC
#include "3rdparty/thorvg/inc/thorvg.h"
#include "Drawer2D.h"

using namespace osgVerse;

namespace
{
    static const float PI = 3.14159265358979323846f;

    static inline uint8_t colorByte(float v)
    {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return (uint8_t)(v * 255.0f + 0.5f);
    }

    static osg::Matrix3 identity3()
    {
        osg::Matrix3 m; m.makeIdentity(); return m;
    }

    static osg::Matrix3 translate3(float x, float y)
    {
        osg::Matrix3 m; m.makeIdentity(); m(2, 0) = x; m(2, 1) = y; return m;
    }

    static osg::Matrix3 scale3(float x, float y)
    {
        osg::Matrix3 m; m.makeIdentity(); m(0, 0) = x; m(1, 1) = y; return m;
    }

    static osg::Matrix3 rotate3(float angle)
    {
        float c = std::cos(angle), s = std::sin(angle);
        osg::Matrix3 m; m.makeIdentity();
        m(0, 0) = c; m(0, 1) = s; m(1, 0) = -s; m(1, 1) = c; return m;
    }

    static osg::Matrix3 multiply3(const osg::Matrix3& a, const osg::Matrix3& b)
    {
        osg::Matrix3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                float v = 0.0f;
                for (int k = 0; k < 3; ++k) v += a(i, k) * b(k, j);
                r(i, j) = v;
            }
        return r;
    }

    // osg::Matrix3 stores a row-vector transform with translation in row 2,
    // while ThorVG reads points as x' = x*e11 + y*e12 + e13, so the matrix is transposed.
    static tvg::Matrix toTvgMatrix(const osg::Matrix3& m)
    {
        tvg::Matrix t;
        t.e11 = m(0, 0); t.e12 = m(1, 0); t.e13 = m(2, 0);
        t.e21 = m(0, 1); t.e22 = m(1, 1); t.e23 = m(2, 1);
        t.e31 = 0.0f;    t.e32 = 0.0f;    t.e33 = 1.0f;
        return t;
    }

    static tvg::BlendMethod toTvgBlend(Drawer2D::BlendMode m)
    {
        switch (m)
        {
        case Drawer2D::Multiply: return tvg::BlendMethod::Multiply;
        case Drawer2D::Screen: return tvg::BlendMethod::Screen;
        case Drawer2D::Overlay: return tvg::BlendMethod::Overlay;
        case Drawer2D::Darken: return tvg::BlendMethod::Darken;
        case Drawer2D::Lighten: return tvg::BlendMethod::Lighten;
        case Drawer2D::HardLight: return tvg::BlendMethod::HardLight;
        case Drawer2D::SoftLight: return tvg::BlendMethod::SoftLight;
        case Drawer2D::Difference: return tvg::BlendMethod::Difference;
        case Drawer2D::Exclusion: return tvg::BlendMethod::Exclusion;
        case Drawer2D::Plus: return tvg::BlendMethod::Add;
        default: return tvg::BlendMethod::Normal;
        }
    }

    static bool isSupportedBlend(Drawer2D::BlendMode m)
    {
        switch (m)
        {
        case Drawer2D::SourceOver: case Drawer2D::Multiply: case Drawer2D::Screen:
        case Drawer2D::Overlay: case Drawer2D::Darken: case Drawer2D::Lighten:
        case Drawer2D::HardLight: case Drawer2D::SoftLight: case Drawer2D::Difference:
        case Drawer2D::Exclusion: case Drawer2D::Plus: return true;
        default: return false;
        }
    }

    static tvg::FillSpread toTvgSpread(DrawerStyleData::ExtendMode e)
    {
        switch (e)
        {
        case DrawerStyleData::REPEAT: case DrawerStyleData::PAD_REPEAT:
        case DrawerStyleData::REPEAT_PAD: case DrawerStyleData::REPEAT_MIRROR:
            return tvg::FillSpread::Repeat;
        case DrawerStyleData::MIRROR: case DrawerStyleData::PAD_MIRROR:
        case DrawerStyleData::MIRROR_PAD: case DrawerStyleData::MIRROR_REPEAT:
            return tvg::FillSpread::Reflect;
        default: return tvg::FillSpread::Pad;
        }
    }

    static tvg::StrokeCap toTvgCap(int cap)
    {
        switch (cap)
        {
        case 1: return tvg::StrokeCap::Square;
        case 2: case 3: return tvg::StrokeCap::Round;  // 3 is not supported, use the nearest one
        default: return tvg::StrokeCap::Butt;
        }
    }

    static tvg::StrokeJoin toTvgJoin(int join)
    {
        switch (join)
        {
        case 2: case 4: return tvg::StrokeJoin::Round;
        case 3: return tvg::StrokeJoin::Bevel;
        default: return tvg::StrokeJoin::Miter;
        }
    }

    static tvg::Fill* createGradient(const DrawerStyleData& sd)
    {
        tvg::Fill* fill = NULL;
        if (sd.type == DrawerStyleData::LINEAR_GRADIENT)
        {
            tvg::LinearGradient* g = tvg::LinearGradient::gen();
            g->linear(sd.gradient[0], sd.gradient[1], sd.gradient[2], sd.gradient[3]);
            fill = g;
        }
        else if (sd.type == DrawerStyleData::RADIAL_GRADIENT)
        {
            tvg::RadialGradient* g = tvg::RadialGradient::gen();
            // osgVerse stores (start, radius0, end, radius1), ThorVG wants (center, r, focal, fr)
            g->radial(sd.gradient[2], sd.gradient[3], sd.gradient2[1],
                      sd.gradient[0], sd.gradient[1], sd.gradient2[0]);
            fill = g;
        }
        else return NULL;

        std::vector<tvg::Fill::ColorStop> stops;
        for (std::map<float, osg::Vec4f>::const_iterator itr = sd.gradientStops.begin();
             itr != sd.gradientStops.end(); ++itr)
        {
            tvg::Fill::ColorStop stop;
            stop.offset = itr->first;
            stop.r = colorByte(itr->second[0]); stop.g = colorByte(itr->second[1]);
            stop.b = colorByte(itr->second[2]); stop.a = colorByte(itr->second[3]);
            stops.push_back(stop);
        }
        if (!stops.empty()) fill->colorStops(stops.data(), (uint32_t)stops.size());
        fill->spread(toTvgSpread(sd.extending));
        fill->transform(toTvgMatrix(sd.transform));
        return fill;
    }

    static bool applyFill(tvg::Shape* shape, const DrawerStyleData& sd)
    {
        if (sd.type == DrawerStyleData::COLOR)
        {
            shape->fill(colorByte(sd.color[0]), colorByte(sd.color[1]),
                        colorByte(sd.color[2]), colorByte(sd.color[3]));
            return true;
        }

        tvg::Fill* fill = createGradient(sd);
        if (!fill) return false;
        shape->fill(fill); return true;
    }

    static bool applyStroke(tvg::Shape* shape, const DrawerStyleData& sd)
    {
        if (sd.type == DrawerStyleData::COLOR)
        {
            shape->strokeFill(colorByte(sd.color[0]), colorByte(sd.color[1]),
                              colorByte(sd.color[2]), colorByte(sd.color[3]));
            return true;
        }

        tvg::Fill* fill = createGradient(sd);
        if (!fill) return false;
        shape->strokeFill(fill); return true;
    }

    static const uint8_t* getImageRGBA(const osg::Image* image, uint32_t& w, uint32_t& h,
                                       std::vector<uint32_t>& storage)
    {
        if (!image || image->getDataType() != GL_UNSIGNED_BYTE) return NULL;
        w = (uint32_t)image->s(); h = (uint32_t)image->t();
        const uint8_t* src = image->data();
        if (!src || w < 1 || h < 1) return NULL;

        const size_t count = (size_t)w * h;
        if (image->getPixelFormat() == GL_RGBA) return src;
        else if (image->getPixelFormat() == GL_RGB)
        {
            storage.resize(count);
            uint8_t* dst = (uint8_t*)storage.data();
            for (size_t i = 0; i < count; ++i)
            {
                dst[i * 4 + 0] = src[i * 3 + 0]; dst[i * 4 + 1] = src[i * 3 + 1];
                dst[i * 4 + 2] = src[i * 3 + 2]; dst[i * 4 + 3] = 255;
            }
            return dst;
        }
        else if (image->getPixelFormat() == GL_LUMINANCE ||
                 image->getPixelFormat() == GL_ALPHA || image->getPixelFormat() == GL_RED)
        {
            storage.resize(count);
            uint8_t* dst = (uint8_t*)storage.data();
            for (size_t i = 0; i < count; ++i)
            {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
                dst[i * 4 + 3] = src[i];
            }
            return dst;
        }

        OSG_WARN << "[Drawer2D] Unsupported image pixel format: " << image->getPixelFormat() << std::endl;
        return NULL;
    }
}

struct TvgCore : public osg::Referenced
{
    TvgCore() : canvas(NULL), strokeWidth(1.0), startCap(0), endCap(0), join(0),
                dashOffset(0.0), miterLimit(4.0), blend(Drawer2D::SourceOver),
                width(0), height(0), usingImageData(false)
    { transform.makeIdentity(); }

    ~TvgCore() { delete canvas; }

    tvg::SwCanvas* canvas;
    std::vector<uint32_t> buffer;
    std::map<std::string, bool> fonts;

    osg::Matrix3 transform;
    double strokeWidth, dashOffset, miterLimit;
    int startCap, endCap, join;
    Drawer2D::BlendMode blend;
    int width, height; bool usingImageData;
};

namespace
{
    struct DrawerThread : public OpenThreads::Thread
    {
        DrawerThread(Drawer2D* d, Drawer2D::DrawerCallback cb)
        : _drawer(d), _callback(cb), _running(true) {}

        virtual void run()
        {
            while (_drawer.valid() && _running)
            {
                _callback(_drawer.get());
                _running = false; microSleep(15000);
            }
            _drawer->setDrawingInThread(2);
        }

        virtual int cancel()
        {
            _running = false;
            return OpenThreads::Thread::cancel();
        }

    protected:
        osg::observer_ptr<Drawer2D> _drawer;
        Drawer2D::DrawerCallback _callback;
        bool _running;
    };

    static void ensureTvgInitialized(int threads)
    {
        static std::once_flag flag;
        std::call_once(flag, [threads]()
        {
            // 0 disables the internal worker threads and renders on the calling thread,
            // which is safer when many Drawer2D instances are used concurrently.
            tvg::Initializer::init(threads > 0 ? (uint32_t)threads : 0);
        });
    }

    static void addPaint(TvgCore* core, tvg::Paint* paint)
    {
        if (!core || !core->canvas || !paint) return;
        paint->blend(toTvgBlend(core->blend));
        if (core->canvas->add(paint) != tvg::Result::Success)
        {
            OSG_WARN << "[Drawer2D] Failed to add paint to canvas" << std::endl;
            paint->unref();
        }
    }

    static tvg::Shape* clonePath(tvg::Shape* src)
    {
        const tvg::PathCommand* cmds = NULL; uint32_t cmdsCnt = 0;
        const tvg::Point* pts = NULL; uint32_t ptsCnt = 0;
        if (src->path(&cmds, &cmdsCnt, &pts, &ptsCnt) != tvg::Result::Success) return NULL;

        tvg::Shape* shape = tvg::Shape::gen();
        shape->appendPath(cmds, cmdsCnt, pts, ptsCnt);
        return shape;
    }

    static void addImageFill(TvgCore* core, tvg::Shape* shape, const DrawerStyleData& sd)
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        if (shape->bounds(&x, &y, &w, &h) != tvg::Result::Success || w < 0.01f || h < 0.01f)
        {
            shape->unref(); return;
        }

        uint32_t iw = 0, ih = 0; std::vector<uint32_t> storage;
        const uint8_t* rgba = getImageRGBA(sd.image.get(), iw, ih, storage);
        if (!rgba)
        {
            applyFill(shape, DrawerStyleData(sd.color, true)); addPaint(core, shape); return;
        }

        tvg::Picture* picture = tvg::Picture::gen();
        if (sd.extending != DrawerStyleData::PAD)
            OSG_WARN << "[Drawer2D] Image extend modes other than PAD are not supported" << std::endl;
        if (picture->load((const uint32_t*)rgba, iw, ih, tvg::ColorSpace::ABGR8888S, true)
            != tvg::Result::Success)
        {
            OSG_WARN << "[Drawer2D] Failed to create picture for image style" << std::endl;
            picture->unref(); shape->unref(); return;
        }

        picture->size((float)iw, (float)ih);
        // osg::Image pixels are stored bottom-up, so flip Y while placing the picture
        osg::Matrix3 local = multiply3(scale3(w / (float)iw, -h / (float)ih),
                                       translate3(x, y + h));
        osg::Matrix3 matrix = multiply3(multiply3(local, sd.transform), core->transform);
        picture->transform(toTvgMatrix(matrix));

        tvg::Shape* clipper = clonePath(shape);
        if (clipper)
        {
            clipper->fillRule(tvg::FillRule::EvenOdd);
            // Picture takes its own reference; do NOT unref the clipper here or the
            // picture will keep a dangling pointer and rendering/teardown will crash.
            picture->clip(clipper);
        }
        shape->unref();
        addPaint(core, picture);
    }

    static void applyStrokeState(tvg::Shape* shape, TvgCore* core)
    {
        shape->strokeWidth((float)core->strokeWidth);
        shape->strokeCap(toTvgCap(core->startCap));
        shape->strokeJoin(toTvgJoin(core->join));
        shape->strokeMiterlimit((float)core->miterLimit);
        // TODO: ThorVG needs a dash pattern; Drawer2D does not expose one, so DASH_OFFSET
        // cannot be applied without inventing an API for the dash array.
    }

    static void finishShape(TvgCore* core, tvg::Shape* shape, const DrawerStyleData& sd, bool filled)
    {
        if (filled && sd.type == DrawerStyleData::IMAGE)
        {
            addImageFill(core, shape, sd); return;
        }

        shape->transform(toTvgMatrix(core->transform));
        if (filled)
        {
            if (!applyFill(shape, sd))
            { OSG_WARN << "[Drawer2D] Unsupported fill style: " << sd.type << std::endl; }
        }
        else
        {
            applyStrokeState(shape, core);
            if (!applyStroke(shape, sd))
            {
                OSG_WARN << "[Drawer2D] Image style is not supported for strokes" << std::endl;
                shape->strokeFill(colorByte(sd.color[0]), colorByte(sd.color[1]),
                                  colorByte(sd.color[2]), colorByte(sd.color[3]));
            }
        }
        addPaint(core, shape);
    }

    static std::string wideToUtf8(const std::wstring& w)
    {
        std::string out; out.reserve(w.size() * 3);
        struct Encoder
        {
            static void append(std::string& out, uint32_t cp)
            {
                if (cp <= 0x7F) out.push_back((char)cp);
                else if (cp <= 0x7FF)
                {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                else if (cp <= 0xFFFF)
                {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                else
                {
                    out.push_back((char)(0xF0 | (cp >> 18)));
                    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
            }
        };

        if (sizeof(wchar_t) == 2)  // UTF-16 (Windows)
        {
            for (size_t i = 0; i < w.size(); ++i)
            {
                uint32_t cp = (uint16_t)w[i];
                if (cp >= 0xD800 && cp <= 0xDBFF && (i + 1) < w.size())
                {
                    uint32_t lo = (uint16_t)w[i + 1];
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                    { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
                }
                Encoder::append(out, cp);
            }
        }
        else  // UTF-32 (Linux/macOS)
        {
            for (size_t i = 0; i < w.size(); ++i)
                Encoder::append(out, (uint32_t)w[i]);
        }
        return out;
    }

    static bool setupText(tvg::Text* text, TvgCore* core, float size,
                          const std::string& utf8, const std::string& fontName)
    {
        if (!text || core->fonts.empty()) return false;
        std::string name = fontName;
        if (name.empty() || core->fonts.find(name) == core->fonts.end())
            name = core->fonts.begin()->first;

        if (text->font(name.c_str()) != tvg::Result::Success)
        {
            OSG_WARN << "[Drawer2D] Unable to use font: " << name << std::endl; return false;
        }
        text->size(size); text->text(utf8.c_str()); return true;
    }

    static void addTextPaint(TvgCore* core, const osg::Vec2f& pos, float size,
                             const std::string& utf8, const std::string& fontName,
                             const DrawerStyleData& sd)
    {
        tvg::Text* text = tvg::Text::gen();
        if (!setupText(text, core, size, utf8, fontName)) { text->unref(); return; }

        float ascent = size;
        tvg::TextMetrics metrics;
        if (text->metrics(metrics) == tvg::Result::Success) ascent = metrics.ascent;

        if (!sd.filled)
        {
            // Outline only. ThorVG's outline color has no alpha, use paint opacity instead.
            text->outline((float)std::max(0.0, core->strokeWidth),
                          colorByte(sd.color[0]), colorByte(sd.color[1]), colorByte(sd.color[2]));
            text->opacity(colorByte(sd.color[3]));
        }
        else
        {
            if (sd.type == DrawerStyleData::IMAGE || sd.type == DrawerStyleData::LINEAR_GRADIENT ||
                sd.type == DrawerStyleData::RADIAL_GRADIENT)
            {
                // TODO: image patterns are not available for Text, and the gradient coordinate
                // space differs from the previous 2D backend. Fall back to solid color for now.
                if (sd.type != DrawerStyleData::IMAGE)
                    OSG_WARN << "[Drawer2D] Text gradient is not supported yet, using solid color" << std::endl;
            }
            text->fill(colorByte(sd.color[0]), colorByte(sd.color[1]), colorByte(sd.color[2]));
            text->opacity(colorByte(sd.color[3]));
        }

        // ThorVG positions text with its top-left corner, while osgVerse callers use the baseline
        osg::Matrix3 local = translate3(pos[0], pos[1] - ascent);
        text->transform(toTvgMatrix(multiply3(local, core->transform)));
        addPaint(core, text);
    }

    static osg::Vec4 computeTextBBox(TvgCore* core, const std::string& utf8, float size,
                                     const std::string& fontName, bool wideStyle)
    {
        if (!core || core->fonts.empty()) return osg::Vec4();
        tvg::Text* text = tvg::Text::gen();
        if (!setupText(text, core, size, utf8, fontName)) { text->unref(); return osg::Vec4(); }

        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        tvg::TextMetrics metrics;
        bool valid = (text->bounds(&x, &y, &w, &h) == tvg::Result::Success);
        float ascent = size, descent = 0.0f, lineGap = 0.0f;
        if (text->metrics(metrics) == tvg::Result::Success)
        { ascent = metrics.ascent; descent = metrics.descent; lineGap = metrics.linegap; }
        text->unref();
        if (!valid) return osg::Vec4();

        // Keep the previous backend's conventions so existing callers do not need changes
        if (wideStyle) return osg::Vec4(x, y - ascent, w + ascent, h + lineGap + size);
        else return osg::Vec4(x, y - ascent + descent, w, h + lineGap + size);
    }
}

Drawer2D::Drawer2D() : _drawingInThread(0), _drawing(false)
{ _tvgData = new TvgCore; _thread = NULL; }

Drawer2D::Drawer2D(const Drawer2D& img, const osg::CopyOp& op)
:   osg::Image(img, op), _tvgData(img._tvgData), _thread(img._thread),
    _drawingInThread(img._drawingInThread), _drawing(img._drawing)
{}

Drawer2D::Drawer2D(const osg::Image& img, const osg::CopyOp& op)
:   osg::Image(img, op), _drawingInThread(0), _drawing(false)
{ _tvgData = new TvgCore; _thread = NULL; }

void Drawer2D::setDrawingInThread(int b)
{
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
    _drawingInThread = b;  // 0: not drawing, 1: before draw, 2: after draw
}

bool Drawer2D::testDrawingInThread(int b)
{
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
    return (_drawingInThread == b) || (_drawingInThread <= 0);
}

bool Drawer2D::startInThread(DrawerCallback cb, bool useCurrentPixels)
{
    bool started = start(useCurrentPixels, 0);
    if (started)
    {
        destroyDrawingThread();
        _thread = new DrawerThread(this, cb);
        setDrawingInThread(1); _thread->start();
    }
    return started;
}

void Drawer2D::destroyDrawingThread()
{
    setDrawingInThread(0);
    if (_thread != NULL)
    {
        _thread->cancel(); _thread->join();
        delete _thread; _thread = NULL;
    }
}

bool Drawer2D::start(bool useCurrentPixels, int threads)
{
    if (!testDrawingInThread(0) || _drawing) return false;

    int w = 1024; if (s() > 1) w = s();
    int h = 1024; if (t() > 1) h = t();
    if (!_tvgData) _tvgData = new TvgCore;
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());

    core->width = w; core->height = h;
    core->buffer.clear(); core->usingImageData = false;

    bool useImage = useCurrentPixels && getDataType() == GL_UNSIGNED_BYTE &&
                    getPixelFormat() == GL_RGBA && data() != NULL;
    uint32_t* target = NULL;
    if (useImage) target = reinterpret_cast<uint32_t*>(data());
    else if (useCurrentPixels)
    {
        core->buffer.resize((size_t)w * h, 0);
        uint8_t* dst = (uint8_t*)core->buffer.data();
        const uint8_t* src = data();
        if (src && getDataType() == GL_UNSIGNED_BYTE)
        {
            size_t count = (size_t)w * h;
            if (getPixelFormat() == GL_RGB)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    dst[i * 4 + 0] = src[i * 3 + 0]; dst[i * 4 + 1] = src[i * 3 + 1];
                    dst[i * 4 + 2] = src[i * 3 + 2]; dst[i * 4 + 3] = 255;
                }
            }
            else if (getPixelFormat() == GL_LUMINANCE ||
                     getPixelFormat() == GL_ALPHA || getPixelFormat() == GL_RED)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
                    dst[i * 4 + 3] = src[i];
                }
            }
            else OSG_WARN << "[Drawer2D] Unable to reuse current pixels" << std::endl;
        }
        target = core->buffer.data(); core->usingImageData = false;
    }
    else
    {
        core->buffer.assign((size_t)w * h, 0);  // transparent black, same as context->clearAll()
        target = core->buffer.data(); core->usingImageData = false;
    }
    if (!target) { OSG_WARN << "[Drawer2D] Failed to allocate target buffer" << std::endl; return false; }

    ensureTvgInitialized(threads);
    core->canvas = tvg::SwCanvas::gen();
    if (!core->canvas || core->canvas->target(target, (uint32_t)w, (uint32_t)w, (uint32_t)h,
        tvg::ColorSpace::ABGR8888S) != tvg::Result::Success)
    {
        OSG_WARN << "[Drawer2D] Failed to create ThorVG canvas" << std::endl;
        delete core->canvas; core->canvas = NULL; return false;
    }
    core->usingImageData = useImage;
    core->canvas->viewport(0, 0, w, h);

    core->transform.makeIdentity();
    core->strokeWidth = 1.0; core->startCap = core->endCap = 0; core->join = 0;
    core->dashOffset = 0.0; core->miterLimit = 4.0; core->blend = SourceOver;
    _drawing = true; return true;
}

bool Drawer2D::finish()
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!testDrawingInThread(2)) return false;
    else if (_thread != NULL) destroyDrawingThread();

    bool rendered = false; _drawing = false;
    if (core && core->canvas)
    {
        tvg::Result result = core->canvas->update();
        if (result == tvg::Result::Success) result = core->canvas->draw();
        if (result != tvg::Result::Success)
            OSG_WARN << "[Drawer2D] ThorVG rendering failed: " << (int)result << std::endl;
        core->canvas->sync();
        int w = core->width, h = core->height;

        if (core->usingImageData)
        {
            dirty(); rendered = true;  // rendered directly into the image pixels
        }
        else if (!core->buffer.empty())
        {
            const uint8_t* src = (const uint8_t*)core->buffer.data();
            size_t count = (size_t)w * h;
            if (_pixelFormat == GL_RGB)
            {
                if (s() != w || t() != h) allocateImage(w, h, 1, GL_RGB, GL_UNSIGNED_BYTE);
                uint8_t* dst = data();
                for (size_t i = 0; i < count; ++i)
                {
                    dst[i * 3 + 0] = src[i * 4 + 0]; dst[i * 3 + 1] = src[i * 4 + 1];
                    dst[i * 3 + 2] = src[i * 4 + 2];
                }
            }
            else if (_pixelFormat == GL_LUMINANCE || _pixelFormat == GL_ALPHA || _pixelFormat == GL_RED)
            {
                if (s() != w || t() != h) allocateImage(w, h, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);
                uint8_t* dst = data();
                for (size_t i = 0; i < count; ++i) dst[i] = src[i * 4 + 3];
            }
            else
            {
                if (_pixelFormat != GL_RGBA || s() != w || t() != h)
                    allocateImage(w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                memcpy(data(), src, count * 4);
            }
            dirty(); rendered = true;
        }

        delete core->canvas; core->canvas = NULL;
    }
    return rendered;
}

bool Drawer2D::loadFont(const std::string& name, const std::string& file)
{
    std::ifstream fin(file.c_str(), std::ios::in | std::ios::binary);
    if (!fin)
    { OSG_WARN << "[Drawer2D] Unable to read font file: " << file << std::endl; return false; }
    return loadFont(name, fin);
}

bool Drawer2D::loadFont(const std::string& name, std::istream& fin)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core) return false;

    std::string buffer((std::istreambuf_iterator<char>(fin)),
                       std::istreambuf_iterator<char>());
    if (buffer.empty())
    { OSG_WARN << "[Drawer2D] Unable to read font stream: " << name << std::endl; return false; }

    tvg::Result r = tvg::Text::load(name.c_str(), buffer.data(), (uint32_t)buffer.size(), "ttf", true);
    if (r != tvg::Result::Success)
    {
        OSG_WARN << "[Drawer2D] Unable to load font: " << name << " (result: " << (int)r << ")" << std::endl;
        return false;
    }
    core->fonts[name] = true; return true;
}

void Drawer2D::drawText(const osg::Vec2f pos, float size, const std::wstring& text,
                        const std::string& fontName, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;
    if (core->fonts.empty())
    { OSG_WARN << "[Drawer2D] Unable to draw text without any font" << std::endl; return; }
    addTextPaint(core, pos, size, wideToUtf8(text), fontName, sd);
}

void Drawer2D::drawUtf8Text(const osg::Vec2f pos, float size, const std::string& text,
                            const std::string& fontName, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;
    if (core->fonts.empty())
    { OSG_WARN << "[Drawer2D] Unable to draw text without any font" << std::endl; return; }
    addTextPaint(core, pos, size, text, fontName, sd);
}

osg::Vec4 Drawer2D::getTextBoundingBox(const std::wstring& text, float size, const std::string& fontName)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return osg::Vec4();
    return computeTextBBox(core, wideToUtf8(text), size, fontName, true);
}

osg::Vec4 Drawer2D::getUtf8TextBoundingBox(const std::string& text, float size, const std::string& fontName)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return osg::Vec4();
    return computeTextBBox(core, text, size, fontName, false);
}

void Drawer2D::drawLine(const osg::Vec2f p0, const osg::Vec2f p1, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    tvg::Shape* shape = tvg::Shape::gen();
    shape->moveTo(p0[0], p0[1]); shape->lineTo(p1[0], p1[1]);
    finishShape(core, shape, sd, false);  // a single line is always stroked
}

void Drawer2D::drawPolyline(const std::vector<osg::Vec2f>& points,
                            bool closed, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;
    if (points.size() < 2)
    { OSG_WARN << "[Drawer2D] Too few points for drawing polyline" << std::endl; return; }

    bool close = closed && points.size() > 2;
    tvg::Shape* shape = tvg::Shape::gen();
    shape->moveTo(points[0][0], points[0][1]);
    for (size_t i = 1; i < points.size(); ++i) shape->lineTo(points[i][0], points[i][1]);
    if (close) shape->close();
    finishShape(core, shape, sd, sd.filled && close);
}

void Drawer2D::drawCircle(const osg::Vec2f pos, float r1, float r2, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    float ry = (r2 > 0.0f) ? r2 : r1;
    tvg::Shape* shape = tvg::Shape::gen();
    shape->appendCircle(pos[0], pos[1], r1, ry);
    finishShape(core, shape, sd, sd.filled);
}

void Drawer2D::drawArc(const osg::Vec2f pos, float r1, float r2, float start, float sweep,
                       int asChordOrPie, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    float rx = r1, ry = (r2 > 0.0f) ? r2 : r1;
    float a0 = start * (PI / 180.0f), a1 = (start + sweep) * (PI / 180.0f);
    float cx = pos[0], cy = pos[1];
    tvg::Shape* shape = tvg::Shape::gen();
    shape->moveTo(cx + rx * std::cos(a0), cy + ry * std::sin(a0));

    int segments = (int)std::ceil(std::fabs(sweep) / 90.0f);
    if (segments < 1) segments = 1;
    float da = (a1 - a0) / segments;
    float k = 4.0f / 3.0f * std::tan(da / 4.0f);
    for (int i = 0; i < segments; ++i)
    {
        float s = a0 + da * i, e = s + da;
        float cs = std::cos(s), ss = std::sin(s), ce = std::cos(e), se = std::sin(e);
        float c1x = cx + rx * (cs - k * ss), c1y = cy + ry * (ss + k * cs);
        float c2x = cx + rx * (ce + k * se), c2y = cy + ry * (se - k * ce);
        float ex = cx + rx * ce, ey = cy + ry * se;
        shape->cubicTo(c1x, c1y, c2x, c2y, ex, ey);
    }

    if (asChordOrPie == 2)
    { shape->lineTo(cx, cy); shape->close(); }
    else if (asChordOrPie == 1 || sd.filled)
        shape->close();
    finishShape(core, shape, sd, sd.filled);
}

void Drawer2D::drawRectangle(const osg::Vec4f r, float rx, float ry, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    tvg::Shape* shape = tvg::Shape::gen();
    shape->appendRect(r[0], r[1], r[2], r[3], rx, ry);
    finishShape(core, shape, sd, sd.filled);
}

void Drawer2D::drawPolygon(const std::vector<std::vector<osg::Vec2f>>& polygon, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    tvg::Shape* shape = tvg::Shape::gen();
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const std::vector<osg::Vec2f>& ring = polygon[i];
        if (ring.size() < 3) continue;
        shape->moveTo(ring[0][0], ring[0][1]);
        for (size_t j = 1; j < ring.size(); ++j) shape->lineTo(ring[j][0], ring[j][1]);
        shape->close();
    }
    shape->fillRule(tvg::FillRule::EvenOdd);
    finishShape(core, shape, sd, sd.filled);
}

void Drawer2D::drawPath(const std::vector<PathData>& path, const StyleData& sd)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;
    if (path.size() < 2)
    { OSG_WARN << "[Drawer2D] Too few points for drawing path" << std::endl; return; }

    tvg::Shape* shape = tvg::Shape::gen();
    for (size_t i = 0; i < path.size(); ++i)
    {
        const PathData& pd = path[i];
        if (pd.isCubic)
            shape->cubicTo(pd.control0[0], pd.control0[1], pd.control1[0], pd.control1[1],
                           pd.pos[0], pd.pos[1]);
        else if (pd.isMoving) shape->moveTo(pd.pos[0], pd.pos[1]);
        else shape->lineTo(pd.pos[0], pd.pos[1]);
    }
    finishShape(core, shape, sd, sd.filled);
}

void Drawer2D::setStrokeOption(StrokeOption opt, double v)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    switch (opt)
    {
    case WIDTH: core->strokeWidth = v; break;
    case COLOR:  // NOTE: kept for compatibility, shapes/text keep using the style color
        break;
    case START_CAP: core->startCap = (int)v; break;
    case END_CAP: core->endCap = (int)v; break;
    case JOIN: core->join = (int)v; break;
    case DASH_OFFSET: core->dashOffset = v; break;  // TODO: needs a dash pattern API
    case AUTO_JOIN_LIMIT: core->miterLimit = v; break;
    default: OSG_WARN << "[Drawer2D] Unsupported stroke option: " << opt << std::endl; break;
    }
}

double Drawer2D::getStrokeOption(StrokeOption opt) const
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return -1.0;

    switch (opt)
    {
    case WIDTH: return core->strokeWidth;
    case JOIN: return (double)core->join;
    case DASH_OFFSET: return core->dashOffset;
    case AUTO_JOIN_LIMIT: return core->miterLimit;
    default: OSG_WARN << "[Drawer2D] Unsupported stroke option: " << opt << std::endl; break;
    }
    return -1.0;
}

void Drawer2D::translate(const osg::Vec2& pos, bool postMult)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    osg::Matrix3 m = translate3(pos[0], pos[1]);
    if (postMult) core->transform = multiply3(core->transform, m);
    else core->transform = multiply3(m, core->transform);
}

void Drawer2D::scale(const osg::Vec2& scale, bool postMult)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    osg::Matrix3 m = scale3(scale[0], scale[1]);
    if (postMult) core->transform = multiply3(core->transform, m);
    else core->transform = multiply3(m, core->transform);
}

void Drawer2D::rotate(float angle, bool postMult)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    osg::Matrix3 m = rotate3(angle);
    if (postMult) core->transform = multiply3(core->transform, m);
    else core->transform = multiply3(m, core->transform);
}

void Drawer2D::setTransform(const osg::Matrix3& transform)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (core && core->canvas && _drawing) core->transform = transform;
}

osg::Matrix3 Drawer2D::getTransform() const
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (core && core->canvas && _drawing) return core->transform;
    return identity3();
}

void Drawer2D::clear(const osg::Vec4f& r)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    // The previous backend's clear erased what was already drawn; drop the pending ThorVG paints.
    core->canvas->remove();
    uint32_t* target = core->usingImageData ? reinterpret_cast<uint32_t*>(data())
                                            : core->buffer.data();
    if (!target) return;

    int w = core->width, h = core->height;
    if (r[2] > 0.0f && r[3] > 0.0f)
    {
        int x0 = std::max(0, (int)r[0]), y0 = std::max(0, (int)r[1]);
        int x1 = std::min(w, (int)(r[0] + r[2])), y1 = std::min(h, (int)(r[1] + r[3]));
        for (int y = y0; y < y1; ++y)
            memset(target + (size_t)y * w + x0, 0, (size_t)(x1 - x0) * 4);
    }
    else
        memset(target, 0, (size_t)w * h * 4);
}

void Drawer2D::fillBackground(const osg::Vec4f& c)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;
    if (c[3] <= 0.0f) return;  // fully transparent, nothing to blend

    tvg::Shape* shape = tvg::Shape::gen();
    shape->appendRect(0.0f, 0.0f, (float)core->width, (float)core->height);
    shape->fill(colorByte(c[0]), colorByte(c[1]), colorByte(c[2]), colorByte(c[3]));
    addPaint(core, shape);
}

void Drawer2D::setBlendOperator(BlendMode value)
{
    TvgCore* core = static_cast<TvgCore*>(_tvgData.get());
    if (!core || !core->canvas || !_drawing) return;

    core->blend = value;
    if (!isSupportedBlend(value))
        OSG_WARN << "[Drawer2D] Blend mode " << (int)value
                 << " is not supported by ThorVG, falling back to SourceOver" << std::endl;
}
