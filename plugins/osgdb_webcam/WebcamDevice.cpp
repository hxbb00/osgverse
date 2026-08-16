#include "WebcamDevice.h"
#include "WebcamCapture.h"

#include <osg/GL>
#include <osg/Notify>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace osgVerse
{
    WebcamDevice::WebcamDevice()
    {
        _serialNumber = "N/A";
        _firmwareVersion = "1.0";
        _deviceName.clear();
        _deviceIndex = 0;
        _width = 640; _height = 480;
        _fps = 30.0f; _pixelFormatHint = 0;
        _frameIndex = 0; _stopThread = false;
    }

    WebcamDevice::~WebcamDevice()
    {
        disconnect();
    }

    bool WebcamDevice::configure(const osgDB::Options* opts)
    {
        if (opts)
        {
            std::string value = opts->getPluginStringData("DeviceIndex");
            if (!value.empty()) _deviceIndex = atoi(value.c_str());
            value = opts->getPluginStringData("DeviceName"); if (!value.empty()) _deviceName = value;
            value = opts->getPluginStringData("Width"); if (!value.empty()) _width = atoi(value.c_str());
            value = opts->getPluginStringData("Width"); if (!value.empty()) _height = atoi(value.c_str());
            value = opts->getPluginStringData("FrameRate"); if (!value.empty()) _fps = (float)atof(value.c_str());

            value = opts->getPluginStringData("PixelFormat");
            if (!value.empty())
            {
                if (value == "RGB" || value == "rgb" || value == "Rgb") _pixelFormatHint = 1;
                else if (value == "Gray" || value == "GRAY" ||
                         value == "Grey" || value == "grey") _pixelFormatHint = 2;
                else _pixelFormatHint = 0;
            }
        }
        return true;
    }

    bool WebcamDevice::connect(const osgDB::Options* opts)
    {
        if (isStreaming(StreamType::All)) stopAllStreams();
        if (_capture) { _capture->close(); _capture.reset(); }
        setState(DeviceState::Disconnected);

        configure(opts);
        _capture.reset(WebcamCapture::create());
        if (!_capture)
        {
            OSG_WARN << "[osgVerse::WebcamDevice] Unsupported platform "
                     << "(only Windows and Linux are supported)." << std::endl;
            setState(DeviceState::Error); return false;
        }

        if (!_capture->open(_deviceIndex, _deviceName, _width, _height, _fps, _pixelFormatHint))
        {
            OSG_WARN << "[osgVerse::WebcamDevice] Failed to open device " << _deviceIndex
                     << " (" << _width << "x" << _height << ")." << std::endl;
            _capture.reset(); setState(DeviceState::Error); return false;
        }

        _modelName = _capture->deviceName();
        _width = _capture->width(); _height = _capture->height();
        resetStats(); setState(DeviceState::Connected);
        OSG_NOTICE << "[osgVerse::WebcamDevice] Connected: " << _modelName
                   << " (" << _width << "x" << _height << ")." << std::endl;
        return true;
    }

    bool WebcamDevice::disconnect()
    {
        if (isStreaming(StreamType::All)) stopAllStreams();
        if (_capture) { _capture->close(); _capture.reset(); }
        setState(DeviceState::Disconnected);
        return true;
    }

    bool WebcamDevice::supports(StreamType type) const
    { return (type == StreamType::RGB); }

    bool WebcamDevice::getCalibration(Calibration& out) const
    {
        out = Calibration();
        return false;   // plain webcams carry no calibration data
    }

    bool WebcamDevice::startStream(StreamType mask)
    {
        int maskBits = (int)mask;
        if (!(maskBits & (int)StreamType::RGB)) return false;
        if (isStreaming(StreamType::RGB)) return true;
        if (!_capture || getState() != DeviceState::Connected) return false;

        _stopThread = false;
        _captureThread = std::thread(&WebcamDevice::captureLoop, this);
        updateActiveStreams(StreamType::RGB, StreamType::Unknown);
        setState(DeviceState::Streaming);
        return true;
    }

    bool WebcamDevice::stopStream(StreamType mask)
    {
        int maskBits = (int)mask;
        if (!(maskBits & (int)StreamType::RGB)) return true;

        _stopThread = true;
        if (_capture) _capture->requestStop();
        if (_captureThread.joinable()) _captureThread.join();

        updateActiveStreams(StreamType::Unknown, StreamType::RGB);
        if (getState() == DeviceState::Streaming) setState(DeviceState::Connected);
        return true;
    }

    void WebcamDevice::captureLoop()
    {
        WebcamFrame frame;
        while (!_stopThread)
        {
            if (!_capture->readFrame(frame)) { if (!_stopThread) reportError(StreamType::RGB); break; }
            if (_stopThread) break;

            const unsigned int w = (unsigned int)frame.width;
            const unsigned int h = (unsigned int)frame.height;
            const unsigned int channels = (unsigned int)frame.channels;
            if (w == 0 || h == 0 || channels == 0 || channels > 3 || frame.data == NULL)
            { reportError(StreamType::RGB); continue; }

            const unsigned int rowBytes = w * channels;
            const unsigned int stride = (frame.stride > 0) ? (unsigned int)frame.stride : rowBytes;
            const GLenum glFormat = (channels == 1) ? GL_LUMINANCE : GL_RGB;
            if (!_cacheImage || (_cacheImage.valid() &&
                                 (_cacheImage->s() != w || _cacheImage->t() != h)))
            {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->setImage(
                    (int)w, (int)h, 1, glFormat, glFormat, GL_UNSIGNED_BYTE,
                    new unsigned char[(size_t)rowBytes * h], osg::Image::USE_NEW_DELETE);
                _cacheImage = image;
            }

            const unsigned char* src = frame.data;
            unsigned char* dst = _cacheImage->data();
            for (unsigned int y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * stride, rowBytes);

            osg::ref_ptr<ImageFrame> f = new ImageFrame;
            f->timestamp = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            f->frameIndex = (unsigned int)_frameIndex++;
            f->image = _cacheImage; _cacheImage->dirty();
            notifyImage(StreamType::RGB, f.get());
        }
        _stopThread = true;
    }
}
