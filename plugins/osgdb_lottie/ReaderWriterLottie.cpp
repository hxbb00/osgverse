#include <osg/ImageStream>
#include <osg/BoundsChecking>
#include <osg/GL>
#include <osg/Notify>
#include <osg/Timer>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <OpenThreads/Mutex>
#include <OpenThreads/Thread>

#include <rlottie/rlottie_capi.h>
#include <rlottie/lottie/zip/zip.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

/** An animated image stream backed by rlottie. Frames are rasterized on a worker thread
    (ARGB32 premultiplied), converted to plain RGBA and published as the image data, so that
    a normal osg::Texture2D can display it with the default alpha blending. */
class LottieImageStream : public osg::ImageStream, public OpenThreads::Thread
{
public:
    LottieImageStream()
        : _animation(NULL), _width(0), _height(0), _totalFrames(0), _frameRate(30.0),
          _length(0.0), _currentTime(0.0), _timeMultiplier(1.0), _done(false),
          _front(0), _lastFrame((size_t)-1)
    {
        _status = PAUSED;
    }

    virtual Object* cloneType() const { return new LottieImageStream(); }
    virtual Object* clone(const osg::CopyOp&) const { return new LottieImageStream(); }
    virtual bool isSameKindAs(const Object* obj) const
    { return dynamic_cast<const LottieImageStream*>(obj) != NULL; }
    virtual const char* libraryName() const { return "osgVerse"; }
    virtual const char* className() const { return "LottieImageStream"; }

    /** Load animation from JSON text. key is used by rlottie for model caching */
    bool open(const std::string& data, const std::string& key, const std::string& resourcePath)
    {
        if (_animation) return true;
        _animation = lottie_animation_from_data(data.c_str(), key.c_str(), resourcePath.c_str());
        if (!_animation) return false;

        size_t w = 0, h = 0;
        lottie_animation_get_size(_animation, &w, &h);
        if (w < 1 || h < 1) return false;
        _width = (int)w; _height = (int)h;
        _totalFrames = lottie_animation_get_totalframe(_animation);
        _frameRate = lottie_animation_get_framerate(_animation);
        _length = lottie_animation_get_duration(_animation);
        if (_totalFrames < 1) _totalFrames = 1;
        if (_frameRate <= 0.0) _frameRate = 30.0;
        if (_length <= 0.0) _length = (double)_totalFrames / _frameRate;

        _argb.resize((size_t)_width * _height);
        _frames[0].resize((size_t)_width * _height * 4);
        _frames[1].resize((size_t)_width * _height * 4);
        setImage(_width, _height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
                 _frames[0].data(), osg::Image::NO_DELETE);
        renderFrame(0);
        _status = PLAYING;
        start();
        return true;
    }

    virtual void play()
    {
        if (!isRunning()) start();
        _status = PLAYING;
    }

    virtual void pause() { _status = PAUSED; }

    virtual void rewind()
    {
        setReferenceTime(0.0);
        if (!isRunning()) start();
        _status = PLAYING;
    }

    virtual void quit(bool waitForThreadToExit = true)
    {
        _done = true;
        if (isRunning() && waitForThreadToExit) join();
    }

    virtual void setReferenceTime(double time)
    {
        double clamped = osg::clampBetween(time, 0.0, _length);
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
            _currentTime = clamped;
        }
        size_t frame = frameOf(clamped);
        if (frame != _lastFrame) renderFrame(frame);
    }

    virtual double getReferenceTime() const { return _currentTime; }
    virtual double getCurrentTime() const { return _currentTime; }
    virtual double getLength() const { return _length; }
    virtual double getFrameRate() const { return _frameRate; }
    virtual bool isImageTranslucent() const { return true; }

    virtual void setTimeMultiplier(double m)
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
        if (m > 0.0) _timeMultiplier = m;
    }
    virtual double getTimeMultiplier() const { return _timeMultiplier; }

    virtual void run()
    {
        double last = osg::Timer::instance()->time_s();
        while (!_done)
        {
            double now = osg::Timer::instance()->time_s();
            double dt = now - last; last = now;
            if (_status == PLAYING)
            {
                double t = 0.0;
                {
                    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
                    _currentTime += dt * _timeMultiplier;
                    t = _currentTime;
                }
                if (t >= _length)
                {
                    if (getLoopingMode() == LOOPING)
                        t = std::fmod(t, _length);
                    else
                        _status = PAUSED;

                    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
                    _currentTime = t;
                }

                size_t frame = frameOf(t);
                if (frame != _lastFrame) renderFrame(frame);

                double spent = osg::Timer::instance()->time_s() - now;
                double remain = 1.0 / _frameRate - spent;
                if (remain > 0.001) OpenThreads::Thread::microSleep((unsigned)(remain * 1000000.0));
                else OpenThreads::Thread::microSleep(1000);
            }
            else
                OpenThreads::Thread::microSleep(10000);
        }
    }

protected:
    virtual ~LottieImageStream()
    {
        quit(true);
        if (_animation) lottie_animation_destroy(_animation);
    }

    size_t frameOf(double time) const
    {
        size_t frame = (size_t)(time * _frameRate);
        return frame < _totalFrames ? frame : _totalFrames - 1;
    }

    void renderFrame(size_t frame)
    {
        if (!_animation || _width < 1 || _height < 1) return;
        if (frame >= _totalFrames) frame = _totalFrames - 1;
        std::fill(_argb.begin(), _argb.end(), 0);
        lottie_animation_render(_animation, frame, _argb.data(), (size_t)_width, (size_t)_height,
                                (size_t)_width * 4);

        // rlottie writes ARGB32 premultiplied, top-down; OSG expects RGBA bottom-up
        const int back = 1 - _front;
        unsigned char* dst = _frames[back].data();
        for (int y = 0; y < _height; ++y)
        {
            const uint32_t* src = _argb.data() + (size_t)(_height - 1 - y) * _width;
            unsigned char* row = dst + (size_t)y * _width * 4;
            for (int x = 0; x < _width; ++x)
            {
                uint32_t p = src[x];
                unsigned a = (p >> 24) & 0xFF;
                unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                if (a > 0 && a < 255)
                { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
                row[x * 4 + 0] = (unsigned char)r;
                row[x * 4 + 1] = (unsigned char)g;
                row[x * 4 + 2] = (unsigned char)b;
                row[x * 4 + 3] = (unsigned char)a;
            }
        }

        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
            _front = back;
            setData(dst, osg::Image::NO_DELETE);
            _lastFrame = frame;
        }
        dirty();
    }

    Lottie_Animation* _animation;
    int _width, _height;
    size_t _totalFrames;
    double _frameRate, _length, _currentTime, _timeMultiplier;
    bool _done;
    int _front;
    size_t _lastFrame;
    std::vector<uint32_t> _argb;
    std::vector<unsigned char> _frames[2];
    mutable OpenThreads::Mutex _mutex;
};

namespace
{
    bool looksLikeLottie(const std::string& text)
    {
        if (text.empty()) return false;
        if (text.find("\"layers\"") == std::string::npos) return false;
        return text.find("\"v\"") != std::string::npos ||
               text.find("\"fr\"") != std::string::npos ||
               text.find("\"ip\"") != std::string::npos;
    }

    bool looksLikeZip(const std::string& data)
    { return data.size() > 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0; }

    std::string readFileData(const std::string& fileName)
    {
        std::ifstream fin(fileName.c_str(), std::ios::in | std::ios::binary);
        if (!fin) return std::string();
        return std::string((std::istreambuf_iterator<char>(fin)),
                           std::istreambuf_iterator<char>());
    }

    /** Unpack a dotLottie (.lottie zip) package and return its animation JSON. rlottie's own
        zip handling passes a non NUL-terminated buffer to the JSON parser, which crashes on
        valid packages, so the archive is unpacked here and rlottie always gets plain text. */
    bool extractDotLottie(const std::string& data, std::string& json)
    {
        struct zip_t* zip = zip_stream_open(data.data(), data.size(), 0, 'r');
        if (!zip) return false;

        bool found = false;
        for (size_t i = 0; !found; ++i)
        {
            if (zip_entry_openbyindex(zip, i) != 0) break;
            const char* name = zip_entry_name(zip);
            std::string entry = name ? name : "";
            std::string lower = osgDB::convertToLowerCase(entry);
            if (osgDB::getLowerCaseFileExtension(lower) == "json" && lower != "manifest.json")
            {
                void* buffer = NULL; size_t size = 0;
                if (zip_entry_read(zip, &buffer, &size) > 0 && buffer)
                {
                    json.assign((const char*)buffer, size);
                    found = !json.empty();
                    free(buffer);
                }
            }
            zip_entry_close(zip);
        }
        zip_stream_close(zip);
        return found;
    }
}

class ReaderWriterLottie : public osgDB::ReaderWriter
{
public:
    ReaderWriterLottie()
    {
        supportsExtension("verse_lottie", "osgVerse pseudo-loader");
        supportsExtension("lottie", "Lottie animation package (dotLottie or JSON)");
        supportsExtension("json", "Lottie animation (JSON)");
    }

    virtual const char* className() const { return "[osgVerse] Lottie animation reader"; }

    virtual ReadResult readObject(std::istream& fin, const Options* options = NULL) const
    { return readImage(fin, options); }

    virtual ReadResult readObject(const std::string& file, const Options* options = NULL) const
    { return readImage(file, options); }

    virtual ReadResult readImage(std::istream& fin, const Options* = NULL) const
    {
        std::string data((std::istreambuf_iterator<char>(fin)),
                         std::istreambuf_iterator<char>());
        std::string json;
        if (looksLikeZip(data))
        {
            if (!extractDotLottie(data, json)) return ReadResult::ERROR_IN_READING_FILE;
        }
        else if (looksLikeLottie(data))
            json = data;
        else
            return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<LottieImageStream> stream = new LottieImageStream;
        std::ostringstream key; key << "lottie_stream_" << (const void*)stream.get();
        if (!stream->open(json, key.str(), "")) return ReadResult::ERROR_IN_READING_FILE;
        return stream.release();
    }

    virtual ReadResult readImage(const std::string& path, const Options* options) const
    {
        std::string ext, fileName = getRealFileName(path, ext);
        if (fileName.empty()) return ReadResult::FILE_NOT_HANDLED;

        std::string data = readFileData(fileName);
        if (data.empty()) return ReadResult::FILE_NOT_HANDLED;

        std::string json, resourcePath = osgDB::getFilePath(fileName);
        if (looksLikeZip(data))
        {
            if (!extractDotLottie(data, json)) return ReadResult::ERROR_IN_READING_FILE;
        }
        else if (looksLikeLottie(data))
            json = data;
        else
            return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<LottieImageStream> stream = new LottieImageStream;
        if (!stream->open(json, fileName, resourcePath))
            return ReadResult::ERROR_IN_READING_FILE;
        stream->setFileName(path);
        return stream.release();
    }

    /** A lottie file is an image, not a node. Plain .json files are not ours: they belong to
        the 3D tiles plugin, so forward them there when this reader is picked for .json. */
    virtual ReadResult readNode(const std::string& file, const Options* options = NULL) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (ext != "json") return ReadResult::FILE_NOT_HANDLED;

        std::string path = osgDB::findDataFile(file, options);
        if (path.empty()) return ReadResult::FILE_NOT_HANDLED;
        std::string data = readFileData(path);
        if (looksLikeZip(data) || looksLikeLottie(data)) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<osg::Node> node = osgDB::readNodeFile(file + ".verse_tiles", options);
        return node.valid() ? ReadResult(node.get()) : ReadResult(ReadResult::FILE_NOT_HANDLED);
    }

    virtual WriteResult writeImage(const osg::Image&, std::ostream&, const Options*) const
    { return WriteResult::FILE_NOT_HANDLED; }

    virtual WriteResult writeImage(const osg::Image&, const std::string&, const Options*) const
    { return WriteResult::FILE_NOT_HANDLED; }

protected:
    std::string getRealFileName(const std::string& path, std::string& ext) const
    {
        ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return std::string();
        if (ext == "verse_lottie")
        {
            std::string fileName = osgDB::getNameLessExtension(path);
            ext = osgDB::getLowerCaseFileExtension(fileName);
            return osgDB::findDataFile(fileName);
        }
        return osgDB::findDataFile(path);
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse_lottie, ReaderWriterLottie)
