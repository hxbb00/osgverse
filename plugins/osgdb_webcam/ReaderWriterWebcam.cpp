#include <osgDB/FileNameUtils>
#include <osgDB/Registry>

#include "WebcamDevice.h"

/// Pseudo-loader:  osgDB::readObjectFile("device_webcam", options)
///
/// Options:
///   DeviceIndex : index of the webcam device (default = 0)
///   DeviceName  : fuzzy name of the webcam device, overrides DeviceIndex
///   Width       : capture width  (default = 640)
///   Height      : capture height (default = 480)
///   FrameRate   : capture frame rate (default = 30)
///   PixelFormat : "Auto" / "RGB" / "Gray" (default = Auto)
class ReaderWriterWebcam : public osgDB::ReaderWriter
{
public:
    ReaderWriterWebcam()
    {
        supportsExtension("device_webcam", "osgVerse webcam pseudo-loader");
        supportsOption("DeviceIndex", "Index of the webcam device (default = 0)");
        supportsOption("DeviceName", "Fuzzy name of the webcam device (overrides DeviceIndex)");
        supportsOption("Width", "Capture width (default = 640)");
        supportsOption("Height", "Capture height (default = 480)");
        supportsOption("FrameRate", "Capture frame rate (default = 30)");
        supportsOption("PixelFormat", "Pixel format hint: Auto / RGB / Gray (default = Auto)");
    }

    virtual const char* className() const
    { return "[osgVerse] Webcam vision device reader"; }

    virtual ReadResult readObject(const std::string& file, const osgDB::Options* options) const
    {
        if (!acceptsExtension(osgDB::getFileExtension(file)))
            return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<osgVerse::WebcamDevice> dev = new osgVerse::WebcamDevice;
        if (!dev->connect(options)) return ReadResult::ERROR_IN_READING_FILE;
        return dev.get();
    }
};

REGISTER_OSGPLUGIN(device_webcam, ReaderWriterWebcam)
