#ifndef OSGVERSE_PLUGIN_WEBCAM_DEVICE_H
#define OSGVERSE_PLUGIN_WEBCAM_DEVICE_H

#include <osg/Image>
#include <osgDB/Options>
#include <readerwriter/VisionDevice.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace osgVerse
{
    class WebcamCapture;

    /// A vision input device backed by a system webcam.
    /// - Windows: Media Foundation (MFEnumDeviceSources + IMFSourceReader)
    /// - Linux:   Video4Linux2 (V4L2 mmap capture)
    /// Other platforms are not supported yet.
    ///
    /// Options understood by connect()/configure():
    ///   DeviceIndex : index of the camera (default = 0)
    ///   DeviceName  : fuzzy name of the camera, overrides DeviceIndex
    ///   Width       : capture width  (default = 640)
    ///   Height      : capture height (default = 480)
    ///   FrameRate   : capture frame rate (default = 30)
    ///   PixelFormat : "Auto" / "RGB" / "Gray" (default = Auto)
    class WebcamDevice : public VisionInputDevice
    {
    public:
        WebcamDevice(const std::string& name, int idx);
        virtual ~WebcamDevice();

        // VisionInputDevice interface
        virtual const char* getDeviceClassName() const { return "Webcam"; }
        virtual const char* getDeviceModelName() const { return _modelName.c_str(); }
        virtual std::string getSerialNumber() const { return _serialNumber; }
        virtual std::string getFirmwareVersion() const { return _firmwareVersion; }

        virtual bool connect(const osgDB::Options* opts);
        virtual bool disconnect();
        virtual bool configure(const osgDB::Options* opts);
        virtual bool getCalibration(Calibration& out) const;
        virtual bool startStream(StreamType mask);
        virtual bool stopStream(StreamType mask);
        virtual bool supports(StreamType type) const;

    protected:
        virtual void captureLoop();

        osg::ref_ptr<osg::Image> _cacheImage;
        std::unique_ptr<WebcamCapture> _capture;
        std::thread _captureThread;
        std::atomic<bool> _stopThread;
        unsigned long long _frameIndex;

        std::string _modelName;         ///< device friendly name from the backend
        std::string _serialNumber;
        std::string _firmwareVersion;
        std::string _deviceName;        ///< optional fuzzy name filter
        int _deviceIndex, _width, _height;
        int _pixelFormatHint;           ///< 0 = auto, 1 = RGB8, 2 = Gray8
        float _fps;
    };
}

#endif
