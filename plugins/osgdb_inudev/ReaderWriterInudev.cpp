#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include "InuDevice.h"

class ReaderWriterInudev : public osgDB::ReaderWriter
{
public:
    ReaderWriterInudev()
    {
        supportsExtension("device_inu", "osgVerse pseudo-loader");
        supportsOption("ProjectorLevel", "Set projector level before starting (0 = off, 1 = low, 2 = high; default = 0)");
        supportsOption("FrameRate", "Set device target frame rate (default = 10)");
        supportsOption("SensorResolution", "Set device target resolution (0 = full, 1 = binning, 2 = v-binning; default = 0)");
        supportsOption("ChannelRGB", "Set RGB channel number (default = -1 to automatically search)");
        supportsOption("ChannelDepth", "Set depth channel number (default = -1 to automatically search)");
        supportsOption("ChannelIR", "Set IR channel number (default = -1 to automatically search)");
        supportsOption("ChannelFishEye", "Set fish-eye channel number (default = -1 to automatically search)");
        supportsOption("RegisteredChannelRGB", "Set RGB registration channel number (default = -1)");
        supportsOption("RegisteredChannelDepth", "Set depth registration channel number (default = -1)");
        supportsOption("DeviceID", "Set device registration ID (default = empty)");
        supportsOption("IpAddress", "Set device IP address (default = empty)");
        supportsOption("CnnAlgorithm", "Set CNN algorithm to use (default = ObjectDetection); "
                       "Choice: Segmentation/Classification/FaceRecognition/YoloV3/YoloV7/PoseDetection/HandDetection");

        supportsOption("ConfigEnabledRGB", "Enable RGB channel config before starting (default = 0)");
        supportsOption("DigitalGainRGB", "Set digital-gain of RGB (must enable config, default = 85)");
        supportsOption("AnalogGainRGB", "Set analog-gain of RGB (must enable config, default = 0)");
        supportsOption("ExposureValueRGB", "Set exposure of RGB (must enable config, default = 300)");
        supportsOption("AutoExposureRGB", "Enable auto-exposure of RGB (must enable config, default = 0)");

        supportsOption("ConfigEnabledIR", "Enable IR channel config before starting (default = 0)");
        supportsOption("DigitalGainIR", "Set digital-gain of IR (must enable config, default = 85)");
        supportsOption("AnalogGainIR", "Set analog-gain of IR (must enable config, default = 0)");
        supportsOption("ExposureValueIR", "Set exposure of IR (must enable config, default = 300)");
        supportsOption("AutoExposureRIR", "Enable auto-exposure of IR (must enable config, default = 0)");
        supportsOption("UseRoiIR", "Enable region-of-interest of IR (must enable config, default = 0)");
        supportsOption("RoiTopLeftX", "Set ROI top-left X of IR (must enable config, default = 0)");
        supportsOption("RoiTopLeftY", "Set ROI top-left Y of IR (must enable config, default = 0)");
        supportsOption("RoiBottomRightX", "Set ROI bottom-right X of IR (must enable config, default = 0)");
        supportsOption("RoiBottomRightY", "Set ROI bottom-right Y of IR (must enable config, default = 0)");
    }

    virtual const char* className() const
    {
        return "[osgVerse] INUDEV vision device reader";
    }

    /*virtual ReadResult readImage(const std::string& path, const Options* options) const
    {
        std::string ext; std::string fileName = getRealFileName(path, ext);
        std::ifstream in(fileName, std::ios::in | std::ios::binary);
        if (!in) return ReadResult::FILE_NOT_HANDLED;
        
        // TODO
        return NULL;
    }*/

    virtual ReadResult readObject(const std::string& path, const osgDB::Options* options) const
    {
        std::string fileName(path), ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<osgVerse::InuDevice> inu = new osgVerse::InuDevice;
        if (!inu->connect(options)) return ReadResult::FILE_NOT_FOUND;
        if (!inu->configure(options)) return ReadResult::ERROR_IN_READING_FILE; else return inu.get();
    }

protected:
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(device_inu, ReaderWriterInudev)
