#pragma once
#include <osg/io_utils>
#include <osg/Version>
#include <osg/Image>
#include <osg/ImageSequence>
#include <osgDB/Options>

#include <readerwriter/VisionDevice.h>
#include <InuSensorExt.h>
#include <InuStreams.h>

namespace osgVerse
{
    class InuDevice : public VisionInputDevice
    {
    public:
        InuDevice();
        virtual const char* getDeviceClassName() const { return "Inuitive"; }
        virtual const char* getDeviceModelName() const { return _modelName.c_str(); }
        virtual std::string getSerialNumber() const { return _serialNum; }
        virtual std::string getFirmwareVersion() const { return _firmwareVersion; }

        virtual bool connect(const osgDB::Options* opts);
        virtual bool disconnect();

        virtual bool configure(const osgDB::Options* opts);
        virtual bool getCalibration(Calibration& out) const;
        virtual bool startStream(StreamType mask);
        virtual bool stopStream(StreamType mask);
        virtual bool supports(StreamType type) const;

    protected:
        virtual ~InuDevice();
        bool createSensor();
        bool startSensor();
        bool loadCalibrationFromSensor();
        void stopAndTerminateSensor();

        bool controlRGB(bool started);
        bool controlDepth(bool started);
        bool controlIR(bool started);
        bool controlFisheye(bool started);
        bool controlIMU(bool started);
        bool controlSLAM(bool started);
        bool controlPointCloud(bool started);
        bool controlFeatures(bool started);
        bool controlCNN(bool started);
        bool controlTemperature(bool started);

        void onRGBFrame(std::shared_ptr<InuDev::CImageStream>,
                        std::shared_ptr<const InuDev::CImageFrame>, InuDev::CInuError);
        void onDepthFrame(std::shared_ptr<InuDev::CDepthStream>,
                          std::shared_ptr<const InuDev::CImageFrame>, InuDev::CInuError);
        void onIRFrame(std::shared_ptr<InuDev::CStereoImageStream>,
                       std::shared_ptr<const InuDev::CStereoImageFrame>, InuDev::CInuError);
        void onFisheyeFrame(std::shared_ptr<InuDev::CImageStream>,
                            std::shared_ptr<const InuDev::CImageFrame>, InuDev::CInuError);
        void onIMUFrame(std::shared_ptr<InuDev::CImuStream>,
                        std::shared_ptr<const InuDev::CImuFrame>, InuDev::CInuError);
        void onSLAMFrame(std::shared_ptr<InuDev::CSlamStream>,
                         std::shared_ptr<const InuDev::CSlamFrame>, InuDev::CInuError);
        void onPointCloudFrame(std::shared_ptr<InuDev::CPointCloudStream>,
                               std::shared_ptr<const InuDev::CPointCloudFrame>, InuDev::CInuError);
        void onFeaturesFrame(std::shared_ptr<InuDev::CFeaturesTrackingStream>,
                             std::shared_ptr<const InuDev::CFeaturesTrackingFrame>, InuDev::CInuError);
        void onCNNFrame(std::shared_ptr<InuDev::CCnnAppStream>,
                        std::shared_ptr<const InuDev::CCnnAppFrame>, InuDev::CInuError);
        void onTemperatureFrame(std::shared_ptr<InuDev::CTemperaturesStream>,
                                std::shared_ptr<const InuDev::CTemperaturesFrame>, InuDev::CInuError);

        std::shared_ptr<InuDev::CInuSensorExt> _sensor;
        std::shared_ptr<InuDev::CImageStream> _rgbStream;
        std::shared_ptr<InuDev::CImageRegisteredStream> _rgbRegStream;
        std::shared_ptr<InuDev::CDepthStream> _depthStream;
        std::shared_ptr<InuDev::CStereoImageStream> _irStream;
        std::shared_ptr<InuDev::CImageStream> _fisheyeStream;
        std::shared_ptr<InuDev::CImuStream> _imuStream;
        std::shared_ptr<InuDev::CSlamStream> _slamStream;
        std::shared_ptr<InuDev::CPointCloudStream> _pointsStream;
        std::shared_ptr<InuDev::CFeaturesTrackingStream> _featureStream;
        std::shared_ptr<InuDev::CCnnAppStream> _cnnStream;
        std::shared_ptr<InuDev::CTemperaturesStream> _tempStream;

        osg::ref_ptr<osg::Image> _rgbCache, _depthCache, _fisheyeCache, _irCache[2];
        InuDev::CCalibrationData _calibrationData;
        mutable std::mutex _calibrationMutex, _streamMutex;
        
        std::string _modelName, _serialNum, _firmwareVersion, _calibVersion;
        std::string _deviceId, _ipAddress, _cnnMode;
        int _fps, _binning, _rgbChannel, _irChannel, _depthChannel, _fisheyeChannel;
        int _rgbRegistrationChannel, _depthRegistrationChannel;
        bool _calibrationLoaded;
    };
}

