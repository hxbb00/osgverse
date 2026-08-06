#include "InuDevice.h"
using namespace osgVerse;
#define INUERR(ret) static_cast<std::string>(ret)

namespace
{
    bool hasType(VisionInputDevice::StreamType src, VisionInputDevice::StreamType t)
    { return (static_cast<unsigned int>(src) & static_cast<unsigned int>(t)) != 0; }

    InuDev::ESensorResolution toResolution(int code)
    {
        switch (code)
        {
        case 1:  return InuDev::eBinning;
        case 2:  return InuDev::eVerticalBinning;
        case 3:  return InuDev::eFull;
        default: return InuDev::eBinning;
        }
    }

    GLenum toInternalFormat(VisionInputDevice::PixelFormat pf)
    {
        switch (pf)
        {
        case VisionInputDevice::PixelFormat::Gray8:    return GL_LUMINANCE8;
        case VisionInputDevice::PixelFormat::BGR8:     return GL_RGB8;
        case VisionInputDevice::PixelFormat::RGB8:     return GL_RGB8;
        case VisionInputDevice::PixelFormat::BGRA8:    return GL_RGBA8;
        case VisionInputDevice::PixelFormat::RGBA8:    return GL_RGBA8;
        case VisionInputDevice::PixelFormat::Depth16:  return GL_R16;
        case VisionInputDevice::PixelFormat::Depth32F: return GL_R32F;
        default:                                       return GL_R8;
        }
    }
    GLenum toPixelFormat(VisionInputDevice::PixelFormat pf)
    {
        switch (pf)
        {
            case VisionInputDevice::PixelFormat::Gray8:    return GL_LUMINANCE;
            case VisionInputDevice::PixelFormat::BGR8:     return GL_RGB;
            case VisionInputDevice::PixelFormat::RGB8:     return GL_BGR;
            case VisionInputDevice::PixelFormat::BGRA8:    return GL_RGBA;
            case VisionInputDevice::PixelFormat::RGBA8:    return GL_BGRA;
            case VisionInputDevice::PixelFormat::Depth16:  return GL_RED;
            case VisionInputDevice::PixelFormat::Depth32F: return GL_RED;
            default:                                       return GL_RED;
        }
    }

    GLenum toDataType(VisionInputDevice::PixelFormat pf)
    {
        switch (pf)
        {
            case VisionInputDevice::PixelFormat::Depth16:  return GL_UNSIGNED_SHORT;
            case VisionInputDevice::PixelFormat::Depth32F: return GL_FLOAT;
            default:                                       return GL_UNSIGNED_BYTE;
        }
    }

    VisionInputDevice::PixelFormat fromImageFormat(InuDev::CImageStream::EOutputFormat f)
    {
        switch (f)
        {
        case InuDev::CImageStream::eBGRA: return VisionInputDevice::PixelFormat::BGRA8;
        case InuDev::CImageStream::eRGBA: return VisionInputDevice::PixelFormat::RGBA8;
        case InuDev::CImageStream::eBGR:  return VisionInputDevice::PixelFormat::BGR8;
        case InuDev::CImageStream::eRaw:
        case InuDev::CImageStream::eDefault:
        default:
            return VisionInputDevice::PixelFormat::BGR8;  // FIXME: handle YUVY and so on?
        }
    }

    void wrapImage(osg::Image* dst, const InuDev::CImageFrame* src, VisionInputDevice::PixelFormat pixelFmt)
    {
        if (!dst || !src) return;
        int w = src->Width(), h = src->Height();
        GLenum pf = toPixelFormat(pixelFmt), dt = toDataType(pixelFmt);

        if (w <= 0 || h <= 0) return; else dst->dirty();
        if (dst->s() != w || dst->t() != h ||
            dst->getPixelFormat() != pf || dst->getDataType() != dt)
        {
            dst->allocateImage(w, h, 1, pf, dt);
            dst->setInternalTextureFormat(toInternalFormat(pixelFmt));
        }
        memcpy(dst->data(), src->GetData(), dst->getTotalSizeInBytes());
    }
}

InuDevice::InuDevice()
:   _fps(10), _binning(0), _rgbChannel(-1), _irChannel(-1), _depthChannel(-1), _fisheyeChannel(-1),
    _rgbRegistrationChannel(-1), _depthRegistrationChannel(-1), _calibrationLoaded(false)
{
    _rgbCache = new osg::Image; _rgbCache->setName("RGB");
    _depthCache = new osg::Image; _depthCache->setName("Depth");
    _fisheyeCache = new osg::Image; _fisheyeCache->setName("FishEye");
    _irCache[0] = new osg::Image; _irCache[0]->setName("LeftIR");
    _irCache[1] = new osg::Image; _irCache[1]->setName("RightIR");
}

InuDevice::~InuDevice()
{ stopAllStreams(); stopAndTerminateSensor(); }

bool InuDevice::configure(const osgDB::Options* opts)
{
    if (!_sensor) return false;
    if (!opts) return true;

    std::string cfg = opts->getPluginStringData("ConfigEnabledRGB");
    if (atoi(cfg.c_str()) > 0 && _rgbChannel >= 0)
    {
        std::string dGain = opts->getPluginStringData("DigitalGainRGB");
        std::string aGain = opts->getPluginStringData("AnalogGainRGB");
        std::string exposure = opts->getPluginStringData("ExposureValueRGB");
        std::string autoCtrl = opts->getPluginStringData("AutoExposureRGB");

        InuDev::CSensorControlParams p;
        p.DigitalGain = dGain.empty() ? 85.0f : atof(dGain.c_str());
        p.AnalogGain = aGain.empty() ? 0.0f : atof(aGain.c_str());
        p.ExposureTime = exposure.empty() ? 300 : atoi(exposure.c_str());
        p.AutoControl = autoCtrl.empty() ? false : (atoi(autoCtrl.c_str()) > 0);

        // RGB sensor index
        InuDev::CInuError ret = _sensor->SetSensorControlParams(p, _rgbChannel);
        if (ret != InuDev::eOK)
        {
            OSG_NOTICE << "[InuDevice] RGB SetSensorControlParams failed: "
                       << INUERR(ret) << std::endl; return false;
        }
    }

    cfg = opts->getPluginStringData("ConfigEnabledIR");
    if (atoi(cfg.c_str()) > 0 && _irChannel >= 0)
    {
        std::string dGain = opts->getPluginStringData("DigitalGainIR");
        std::string aGain = opts->getPluginStringData("AnalogGainIR");
        std::string exposure = opts->getPluginStringData("ExposureValueIR");
        std::string autoCtrl = opts->getPluginStringData("AutoExposureIR");
        std::string useRoi = opts->getPluginStringData("UseRoiIR");
        std::string roiTopLeft0 = opts->getPluginStringData("RoiTopLeftX");
        std::string roiTopLeft1 = opts->getPluginStringData("RoiTopLeftY");
        std::string roiBottomRight0 = opts->getPluginStringData("RoiBottomRightX");
        std::string roiBottomRight1 = opts->getPluginStringData("RoiBottomRightY");

        InuDev::CSensorControlParams p;
        p.DigitalGain = dGain.empty() ? 85.0f : atof(dGain.c_str());
        p.AnalogGain = aGain.empty() ? 0.0f : atof(aGain.c_str());
        p.ExposureTime = exposure.empty() ? 300 : atoi(exposure.c_str());
        p.AutoControl = autoCtrl.empty() ? false : (atoi(autoCtrl.c_str()) > 0);
        p.Params.UseROI = useRoi.empty() ? false : (atoi(useRoi.c_str()) > 0);
        p.Params.ROITopLeft.X() = roiTopLeft0.empty() ? 0 : atoi(roiTopLeft0.c_str());
        p.Params.ROITopLeft.Y() = roiTopLeft1.empty() ? 0 : atoi(roiTopLeft1.c_str());
        p.Params.ROIBottomRight.X() = roiBottomRight0.empty() ? 0 : atoi(roiBottomRight0.c_str());
        p.Params.ROIBottomRight.Y() = roiBottomRight1.empty() ? 0 : atoi(roiBottomRight1.c_str());

        InuDev::CInuError ret = _sensor->SetSensorControlParams(p, _irChannel);
        if (ret != InuDev::eOK)
        {
            OSG_NOTICE << "[InuDevice] IR SetSensorControlParams failed: "
                       << INUERR(ret) << std::endl; return false;
        }
    }

    std::string projLV = opts->getPluginStringData("ProjectorLevel");  // 0: off, 1: low, 2: high
    if (!projLV.empty())
    {
        switch (atoi(projLV.c_str()))
        {
        case 0: _sensor->SetProjectorLevel(InuDev::eOff, InuDev::ePatterns); break;
        case 1: _sensor->SetProjectorLevel(InuDev::eLow, InuDev::ePatterns); break;
        default: _sensor->SetProjectorLevel(InuDev::eHigh, InuDev::ePatterns); break;
        }
    }
    return true;
}

bool InuDevice::getCalibration(Calibration& out) const
{
    std::lock_guard<std::mutex> lk(_calibrationMutex);
    if (!_calibrationLoaded) return false;

    // Depth sensor index 0 -> VirtualCamera
    const auto& depthSensor = _calibrationData.Sensors.begin()->second;
    out.depthIntrinsics.fx = depthSensor.VirtualCamera.Intrinsic.FocalLength[0];
    out.depthIntrinsics.fy = depthSensor.VirtualCamera.Intrinsic.FocalLength[1];
    out.depthIntrinsics.cx = depthSensor.VirtualCamera.Intrinsic.OpticalCenter[0];
    out.depthIntrinsics.cy = depthSensor.VirtualCamera.Intrinsic.OpticalCenter[1];
    out.depthIntrinsics.distortion = depthSensor.VirtualCamera.Intrinsic.LensDistortion;
    out.depthValid = true;

    auto blIt = _calibrationData.Baselines.find(std::pair<int, int>(0, 1));
    if (blIt != _calibrationData.Baselines.end()) out.baseline = blIt->second;

    // RGB sensor index
    InuDev::CCalibrationData rgbCalib;
    if (_sensor) _sensor->GetCalibrationData(rgbCalib, _rgbChannel);
    if (rgbCalib.Sensors.size() > 2)
    {
        const auto& rgbSensor = rgbCalib.Sensors[2]; bool useVirtual = true;
        if (_calibVersion.size() >= 4)
        {   // CalibrationVersion >= 20 -> use VirtualCamera; else RealCamera
            try
            {
                int major = std::stoi(_calibVersion.substr(2, 2));
                useVirtual = (major >= 20);
            } catch (...) { useVirtual = true; }
        }

        const auto& cam = useVirtual ? rgbSensor.VirtualCamera : rgbSensor.RealCamera;
        out.rgbIntrinsics.fx = cam.Intrinsic.FocalLength[0];
        out.rgbIntrinsics.fy = cam.Intrinsic.FocalLength[1];
        out.rgbIntrinsics.cx = cam.Intrinsic.OpticalCenter[0];
        out.rgbIntrinsics.cy = cam.Intrinsic.OpticalCenter[1];
        out.rgbIntrinsics.distortion = cam.Intrinsic.LensDistortion;
        for (int i = 0; i < 3; ++i)
            out.depthToRgb.rotation[i] = cam.Extrinsic.Rotation[i];
        for (int i = 0; i < 3; ++i)
            out.depthToRgb.translation[i] = cam.Extrinsic.Translation[i];
        out.rgbValid = true; out.dToRgbValid = true;
    }
    return true;
}

bool InuDevice::connect(const osgDB::Options* opts)
{
    resetStats();
    if (opts)
    {
        std::string fpsV = opts->getPluginStringData("FrameRate");
        std::string binV = opts->getPluginStringData("SensorResolution");
        std::string ch, rgbCh = opts->getPluginStringData("RegisteredChannelRGB");
        std::string depthCh = opts->getPluginStringData("RegisteredChannelDepth");
        _deviceId = opts->getPluginStringData("DeviceID");
        _ipAddress = opts->getPluginStringData("IpAddress");
        _cnnMode = opts->getPluginStringData("CnnAlgorithm");
        if (!fpsV.empty()) _fps = atoi(fpsV.c_str());
        if (!binV.empty()) _binning = atoi(binV.c_str());
        if (!rgbCh.empty()) _rgbRegistrationChannel = atoi(rgbCh.c_str());
        if (!depthCh.empty()) _depthRegistrationChannel = atoi(depthCh.c_str());

        ch = opts->getPluginStringData("ChannelRGB"); if (!ch.empty()) _rgbChannel = atoi(ch.c_str());
        ch = opts->getPluginStringData("ChannelDepth"); if (!ch.empty()) _depthChannel = atoi(ch.c_str());
        ch = opts->getPluginStringData("ChannelIR"); if (!ch.empty()) _irChannel = atoi(ch.c_str());
        ch = opts->getPluginStringData("ChannelFishEye"); if (!ch.empty()) _fisheyeChannel = atoi(ch.c_str());
    }

    if (!createSensor()) { setState(DeviceState::Error); return false; }
    if (!startSensor()) { setState(DeviceState::Error); return false; }
    setState(DeviceState::Connected); return true;
}

bool InuDevice::disconnect()
{
    stopAllStreams(); stopAndTerminateSensor();
    setState(DeviceState::Disconnected);
    return true;
}

bool InuDevice::startStream(StreamType mask)
{
    if (!_sensor) return false;
    std::lock_guard<std::mutex> lk(_streamMutex);

    bool ok = true;
    if (hasType(mask, StreamType::RGB)) ok &= controlRGB(true);
    if (hasType(mask, StreamType::Depth)) ok &= controlDepth(true);
    if (hasType(mask, StreamType::IR)) ok &= controlIR(true);
    if (hasType(mask, StreamType::Fisheye)) ok &= controlFisheye(true);
    if (hasType(mask, StreamType::IMU)) ok &= controlIMU(true);
    if (hasType(mask, StreamType::SLAM)) ok &= controlSLAM(true);
    if (hasType(mask, StreamType::PointCloud)) ok &= controlPointCloud(true);
    if (hasType(mask, StreamType::Features)) ok &= controlFeatures(true);
    if (hasType(mask, StreamType::CNN)) ok &= controlCNN(true);
    if (hasType(mask, StreamType::Temperature)) ok &= controlTemperature(true);

    if (isStreaming(StreamType::All)) setState(DeviceState::Streaming);
    return ok;
}

bool InuDevice::stopStream(StreamType mask)
{
    if (!_sensor) return false;
    std::lock_guard<std::mutex> lk(_streamMutex);

    bool ok = true;
    if (hasType(mask, StreamType::RGB)) ok &= controlRGB(false);
    if (hasType(mask, StreamType::Depth)) ok &= controlDepth(false);
    if (hasType(mask, StreamType::IR)) ok &= controlIR(false);
    if (hasType(mask, StreamType::Fisheye)) ok &= controlFisheye(false);
    if (hasType(mask, StreamType::IMU)) ok &= controlIMU(false);
    if (hasType(mask, StreamType::SLAM)) ok &= controlSLAM(false);
    if (hasType(mask, StreamType::PointCloud)) ok &= controlPointCloud(false);
    if (hasType(mask, StreamType::Features)) ok &= controlFeatures(false);
    if (hasType(mask, StreamType::CNN)) ok &= controlCNN(false);
    if (hasType(mask, StreamType::Temperature)) ok &= controlTemperature(false);

    if (!isStreaming(StreamType::All)) setState(DeviceState::Connected);
    return ok;
}

bool InuDevice::supports(StreamType type) const
{
    unsigned int t = static_cast<unsigned int>(type);
    if ((t & static_cast<unsigned int>(StreamType::RGB)) || (t & static_cast<unsigned int>(StreamType::Depth)) ||
        (t & static_cast<unsigned int>(StreamType::IR)) || (t & static_cast<unsigned int>(StreamType::SLAM)) ||
        (t & static_cast<unsigned int>(StreamType::Fisheye)) || (t & static_cast<unsigned int>(StreamType::IMU)) || 
        (t & static_cast<unsigned int>(StreamType::PointCloud)) || (t & static_cast<unsigned int>(StreamType::Features)) ||
        (t & static_cast<unsigned int>(StreamType::CNN)) || (t & static_cast<unsigned int>(StreamType::Temperature)))
    { return true; } else return false;
}

bool InuDevice::createSensor()
{
    _sensor = InuDev::CInuSensorExt::Create(_deviceId, _ipAddress);
    if (!_sensor)
    {
        OSG_NOTICE << "[InuDevice] CInuSensorExt::Create failed" << std::endl;
        return false;
    }

    InuDev::CInuError ret = _sensor->Connect();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Connect sensor failed: 0x" << std::hex
                   << int(ret) << " - " << INUERR(ret) << std::endl;
        _sensor->Terminate(); _sensor->Disconnect();
        _sensor.reset(); return false;
    }

    if (_sensor->GetConnectionState() != InuDev::eConnected)
    {
        OSG_NOTICE << "[InuDevice] sensor not in connected state" << std::endl;
        _sensor->Terminate(); _sensor->Disconnect();
        _sensor.reset(); return false;
    }
    return true;
}

bool InuDevice::startSensor()
{
    InuDev::CDeviceParamsExt params;
    params.FPS = _fps;params.SensorRes = toResolution(_binning);

    InuDev::CHwInformation hwInfo;
    std::vector<InuDev::CDpeParams> dpeInit, dpeStart;
    InuDev::CInuError ret = _sensor->Init(hwInfo, dpeInit, params);
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Init sensor failed: 0x" << std::hex
                   << int(ret) << " - " << INUERR(ret) << std::endl;
        return false;
    }

    std::map<InuDev::CEntityVersion::EEntitiesID, InuDev::CEntityVersion> versions;
    ret = _sensor->GetVersion(versions);
    if (ret == InuDev::eOK)
    {
        std::string nuNum = versions[InuDev::CEntityVersion::eHWRevision].VersionName;
        _firmwareVersion = versions[InuDev::CEntityVersion::eFWVersion].VersionName;
        _serialNum = versions[InuDev::CEntityVersion::eSerialNumber].VersionName;
        _calibVersion = versions[InuDev::CEntityVersion::eCalibrationVersion].VersionName;
        //_bootfixVersion = versions[InuDev::CEntityVersion::eBootfixVersion].VersionName;
        //_usbType = versions[InuDev::CEntityVersion::eUSBSpeed].VersionNum;

        // Pick a friendly model name from the SN prefix
        if (_serialNum.find("BBA") != std::string::npos) _modelName = "R132";
        else if (_serialNum.find("CAA") != std::string::npos) _modelName = "R130";
        else if (_serialNum.find("ABA") != std::string::npos ||
                 _serialNum.find("JHT") != std::string::npos) _modelName = "C158";
        else if (_serialNum.find("ABB") != std::string::npos) _modelName = "C158Tracking";
        else _modelName = "R200Series";
        _modelName += ", " + nuNum;
    }

    InuDev::ESensorResolution depthRes = toResolution(_binning);
    InuDev::ESensorResolution rgbRes = toResolution(_binning);
    InuDev::CStartDeviceParamsExt startParams;
    startParams.VecDpeParams = dpeStart;
    std::string channelInfo;
    for (const auto& ch : hwInfo.GetChannels())
    {
        const uint32_t id = ch.second.ChannelId;
        switch (ch.second.ChannelType)
        {
        case InuDev::eDepthChannel:
            startParams.ChannelControlParam[id].SensorRes = depthRes;
            startParams.ChannelControlParam[id].FPS = _fps;
            if (_depthRegistrationChannel != -1)
            {
                startParams.ChannelControlParam[id].ActivateRegisteredDepth = true;
                startParams.ChannelControlParam[id].RegisteredDepthChannelID =
                    static_cast<uint32_t>(_depthRegistrationChannel);
            }
            if (_depthChannel < 0) _depthChannel = id;
            channelInfo += std::to_string(id) + " = Depth; "; break;
        case InuDev::eDisparityChannel:
            startParams.ChannelControlParam[id].SensorRes = depthRes;
            startParams.ChannelControlParam[id].FPS = _fps;
            channelInfo += std::to_string(id) + " = Disparity; "; break;
        case InuDev::eGeneralCameraChannel:
            startParams.ChannelControlParam[id].SensorRes = rgbRes;
            startParams.ChannelControlParam[id].FPS = _fps;
            if (_rgbChannel < 0) _rgbChannel = id;
            channelInfo += std::to_string(id) + " = Camera; "; break;
        case InuDev::eTrackingChannel:
            startParams.ChannelControlParam[id].SensorRes = InuDev::eFull;
            startParams.ChannelControlParam[id].FPS = 30;
            startParams.ChannelControlParam[id].InterleaveMode = InuDev::eInterleave;
            channelInfo += std::to_string(id) + " = Tracking; "; break;
        case InuDev::eFeaturesTrackingChannel:
            startParams.ChannelControlParam[id].SensorRes = InuDev::eFull;
            startParams.ChannelControlParam[id].FPS = 30;
            startParams.ChannelControlParam[id].InterleaveMode = InuDev::eInterleave;
            channelInfo += std::to_string(id) + " = Features; "; break;
        case InuDev::eStereoChannel:
            startParams.ChannelControlParam[id].SensorRes = toResolution(_binning);
            startParams.ChannelControlParam[id].FPS = _fps;
            startParams.ChannelControlParam[id].InterleaveMode = InuDev::eInterleave;
            if (_irChannel < 0) _irChannel = id; 
            channelInfo += std::to_string(id) + " = Stereo; "; break;
        default:
            channelInfo += std::to_string(id) + " = Unknown; "; break;
        }
    }

    std::map<uint32_t, InuDev::CChannelSize> channelsSize;
    ret = _sensor->Start(channelsSize, startParams);
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Start sensor failed: 0x" << std::hex
                   << int(ret) << " - " << std::string(ret) << std::endl;
        return false;
    }
    OSG_NOTICE << "[InuDevice] Start sensor successfully: " + channelInfo << std::endl;
    return loadCalibrationFromSensor();
}

bool InuDevice::loadCalibrationFromSensor()
{
    if (!_sensor) return false;
    std::lock_guard<std::mutex> lk(_calibrationMutex);
    _sensor->GetCalibrationData(_calibrationData, 0);
    _calibrationLoaded = true; return true;
}

void InuDevice::stopAndTerminateSensor()
{
    if (!_sensor) return;
    InuDev::CInuError ret = _sensor->Stop();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Stop failed: 0x" << std::hex << int(ret)
                   << " - " << INUERR(ret) << std::endl;
    }

    ret = _sensor->Terminate();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Terminate failed: 0x" << std::hex << int(ret)
                   << " - " << INUERR(ret) << std::endl;
    }

    ret = _sensor->Disconnect();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Disconnect failed: 0x" << std::hex << int(ret)
                   << " - " << INUERR(ret) << std::endl;
    }
    _sensor.reset();
}

bool InuDevice::controlRGB(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_rgbStream || _rgbRegStream) return true;
        uint32_t regCh = (_rgbRegistrationChannel == -1)
            ? InuDev::DEFAULT_CHANNEL_ID : static_cast<uint32_t>(_rgbRegistrationChannel);
        InuDev::CImageStream::EOutputFormat outFmt = InuDev::CImageStream::eDefault;
        InuDev::CImageStream::EPostProcessing ppe = InuDev::CImageStream::eNone; // CImageStream::eGammaCorrect;
        if (_rgbRegistrationChannel != -1)
        {
            _rgbRegStream = _sensor->CreateImageRegisteredStream(_rgbChannel, regCh);
            if (!_rgbRegStream) { OSG_NOTICE << "[InuDevice] Failed to create image stream\n"; return false; }
            ret = _rgbRegStream->Init(outFmt, ppe, InuDev::CDepthProperties::EPostProcessing::eDefaultPP);
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init image: " << INUERR(ret) << "\n"; return false; }
            ret = _rgbRegStream->Start();
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start image: " << INUERR(ret) << "\n"; return false; }
            ret = _rgbRegStream->Register([this](auto s, auto f, auto e) { onRGBFrame(s, f, e); });
        }
        else
        {
            _rgbStream = _sensor->CreateImageStream(_rgbChannel);
            if (!_rgbStream) { OSG_NOTICE << "[InuDevice] Failed to create image stream\n"; return false; }
            ret = _rgbStream->Init(outFmt, ppe);
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init image: " << INUERR(ret) << "\n"; return false; }
            ret = _rgbStream->Start();
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start image: " << INUERR(ret) << "\n"; return false; }
            ret = _rgbStream->Register([this](auto s, auto f, auto e) { onRGBFrame(s, f, e); });
        }
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::RGB, StreamType::Unknown);
    }
    else
    {
        if (_rgbRegStream)
        {
            _rgbRegStream->Register(nullptr); _rgbRegStream->Stop();
            _rgbRegStream->Terminate(); _rgbRegStream.reset();
        }
        if (_rgbStream)
        {
            _rgbStream->Register(nullptr); _rgbStream->Stop();
            _rgbStream->Terminate(); _rgbStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::RGB);
    }
    return true;
}

void InuDevice::onRGBFrame(std::shared_ptr<InuDev::CImageStream>,
                           std::shared_ptr<const InuDev::CImageFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<ImageFrame> f = new ImageFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    PixelFormat fmt = fromImageFormat(static_cast<InuDev::CImageStream::EOutputFormat>(frame->Format()));
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    f->image = _rgbCache; wrapImage(f->image.get(), frame.get(), fmt);
    notifyImage(StreamType::RGB, f.get());
}

bool InuDevice::controlDepth(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_depthStream) return true;
        InuDev::CDepthStream::EOutputFormat fmt = InuDev::CDepthStream::eDefault;
        InuDev::CDepthStream::EPostProcessing pp = InuDev::CDepthStream::eDefaultPP;
        
        _depthStream = _sensor->CreateDepthStream(_depthChannel);
        if (!_depthStream) { OSG_NOTICE << "[InuDevice] Failed to create depth stream\n"; return false; }
        ret = _depthStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init depth: " << INUERR(ret) << "\n"; return false; }
        ret = _depthStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start depth: " << INUERR(ret) << "\n"; return false; }

        ret = _depthStream->Register([this](auto s, auto f, auto e) { onDepthFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::Depth, StreamType::Unknown);
    }
    else
    {
        if (_depthStream)
        {
            _depthStream->Register(nullptr); _depthStream->Stop();
            _depthStream->Terminate(); _depthStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::Depth);
    }
    return true;
}

void InuDevice::onDepthFrame(std::shared_ptr<InuDev::CDepthStream>,
                             std::shared_ptr<const InuDev::CImageFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<ImageFrame> f = new ImageFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    f->image = _depthCache; wrapImage(f->image.get(), frame.get(), PixelFormat::Depth16);
    notifyImage(StreamType::Depth, f.get());
}

bool InuDevice::controlIR(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_irStream) return true;
        _irStream = _sensor->CreateStereoImageStream(_irChannel);
        if (!_irStream) { OSG_NOTICE << "[InuDevice] Failed to create IR stream\n"; return false; }
        ret = _irStream->Init(InuDev::CStereoImageStream::eRaw);
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init IR: " << INUERR(ret) << "\n"; return false; }
        ret = _irStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start IR: " << INUERR(ret) << "\n"; return false; }
        ret = _irStream->Register([this](auto s, auto f, auto e) { onIRFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::IR, StreamType::Unknown);
    }
    else
    {
        if (_irStream)
        {
            _irStream->Register(nullptr); _irStream->Stop();
            _irStream->Terminate(); _irStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::IR);
    }
    return true;
}

void InuDevice::onIRFrame(std::shared_ptr<InuDev::CStereoImageStream>,
                          std::shared_ptr<const InuDev::CStereoImageFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<StereoImageFrame> f = new StereoImageFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    if (frame->GetLeftFrame())
    {
        if (!f->left.image) f->left.image = _irCache[0];
        wrapImage(f->left.image.get(), frame->GetLeftFrame(), PixelFormat::Gray8);
    }
    if (frame->GetRightFrame())
    {
        if (!f->right.image) f->right.image = _irCache[1];
        wrapImage(f->right.image.get(), frame->GetRightFrame(), PixelFormat::Gray8);
    }
    notifyStereoImage(StreamType::IR, f.get());
}

bool InuDevice::controlFisheye(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_fisheyeStream) return true;
        _fisheyeStream = _sensor->CreateImageStream(_fisheyeChannel);
        if (!_fisheyeStream) { OSG_NOTICE << "[InuDevice] Failed to create fish-eye stream\n"; return false; }
        ret = _fisheyeStream->Init(InuDev::CImageStream::eDefault, InuDev::CImageStream::eNone);
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init fish-eye: " << INUERR(ret) << "\n"; return false; }
        ret = _fisheyeStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start fish-eye: " << INUERR(ret) << "\n"; return false; }
        ret = _fisheyeStream->Register([this](auto s, auto f, auto e) { onFisheyeFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::Fisheye, StreamType::Unknown);
    }
    else
    {
        if (_fisheyeStream)
        {
            _fisheyeStream->Register(nullptr); _fisheyeStream->Stop();
            _fisheyeStream->Terminate(); _fisheyeStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::Fisheye);
    }
    return true;
}

void InuDevice::onFisheyeFrame(std::shared_ptr<InuDev::CImageStream>,
                               std::shared_ptr<const InuDev::CImageFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<ImageFrame> f = new ImageFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    f->image = _fisheyeCache; wrapImage(f->image.get(), frame.get(), PixelFormat::Gray8);
    notifyImage(StreamType::Fisheye, f.get());
}

bool InuDevice::controlIMU(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_imuStream) return true;
        _imuStream = _sensor->CreateImuStream();
        if (!_imuStream) { OSG_NOTICE << "[InuDevice] Failed to create IMU stream\n"; return false; }
        ret = _imuStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init IMU: " << INUERR(ret) << "\n"; return false; }
        ret = _imuStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start IMU: " << INUERR(ret) << "\n"; return false; }
        ret = _imuStream->Register([this](auto s, auto f, auto e) { onIMUFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::IMU, StreamType::Unknown);
    }
    else
    {
        if (_imuStream)
        {
            _imuStream->Register(nullptr); _imuStream->Stop();
            _imuStream->Terminate(); _imuStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::IMU);
    }
    return true;
}

void InuDevice::onIMUFrame(std::shared_ptr<InuDev::CImuStream>,
                           std::shared_ptr<const InuDev::CImuFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<IMUSampleFrame> f = new IMUSampleFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    auto g = frame->SensorsData.find(InuDev::EImuType::eGyroscope);
    auto a = frame->SensorsData.find(InuDev::EImuType::eAccelerometer);
    if (g != frame->SensorsData.end())
    {
        f->gyro.set(g->second.X(), g->second.Y(), g->second.Z());
        f->hasGyro = true;
    }
    if (a != frame->SensorsData.end())
    {
        f->accel.set(a->second.X(), a->second.Y(), a->second.Z());
        f->hasAccel = true;
    }
    notifyIMU(f.get());
}

bool InuDevice::controlSLAM(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_slamStream) return true;
        _slamStream = _sensor->CreateSlamStream(_rgbChannel);
        if (!_slamStream) { OSG_NOTICE << "[InuDevice] Failed to create SLAM stream\n"; return false; }
        ret = _slamStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init SLAM: " << INUERR(ret) << "\n"; return false; }
        ret = _slamStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start SLAM: " << INUERR(ret) << "n"; return false; }
        ret = _slamStream->Register([this](auto s, auto f, auto e) { onSLAMFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::SLAM, StreamType::Unknown);
    }
    else
    {
        if (_slamStream)
        {
            _slamStream->Register(nullptr); _slamStream->Stop();
            _slamStream->Terminate(); _slamStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::SLAM);
    }
    return true;
}

void InuDevice::onSLAMFrame(std::shared_ptr<InuDev::CSlamStream>,
                         std::shared_ptr<const InuDev::CSlamFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<PoseFrame> f = new PoseFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    std::array<float, 4> Q; std::array<float, 3> T;
    frame->ConvertPose4x4ToQuaternionTranslation(frame->mPose4x4BodyToWorld, Q, T);
    f->orientation.set(Q[0], Q[1], Q[2], Q[3]);
    f->translation.set(T[0], T[1], T[2]);
    f->state = frame->mSlamState; notifyPose(f.get());
}

bool InuDevice::controlPointCloud(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_pointsStream) return true;
        _pointsStream = _sensor->CreatePointCloudStream();
        if (!_pointsStream) { OSG_NOTICE << "[InuDevice] Failed to create point-cloud stream\n"; return false; }
        ret = _pointsStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init point-cloud: " << INUERR(ret) << "\n"; return false; }
        ret = _pointsStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start point-cloud: " << INUERR(ret) << "\n"; return false; }
        ret = _pointsStream->Register([this](auto s, auto f, auto e) { onPointCloudFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::PointCloud, StreamType::Unknown);
    }
    else
    {
        if (_pointsStream)
        {
            _pointsStream->Register(nullptr); _pointsStream->Stop();
            _pointsStream->Terminate(); _pointsStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::PointCloud);
    }
    return true;
}

void InuDevice::onPointCloudFrame(std::shared_ptr<InuDev::CPointCloudStream>,
                                  std::shared_ptr<const InuDev::CPointCloudFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<PointCloudFrame> f = new PointCloudFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    InuDev::CPointCloudFrame::EFormat fmt = static_cast<InuDev::CPointCloudFrame::EFormat>(frame->GetFormat());
    const uint32_t n = frame->GetNumOfPoints(); f->width = n; f->height = 1;
    
    if (fmt == InuDev::CPointCloudFrame::EFormat::e3DPoints)
    {
        const InuDev::CPointCloudFrame::C3DPixel* p = frame->Get3DData();
        f->points = new osg::Vec3Array(n);
        for (uint32_t i = 0; i < n; ++i)
            (*f->points)[i] = osg::Vec3(p[i].X(), p[i].Y(), p[i].Z());
    }
    else if (fmt == InuDev::CPointCloudFrame::EFormat::e3DShortPoints)
    {
        const InuDev::CPointCloudFrame::CPoint3DShortPixel* p = frame->Get3DShortData();
        f->points = new osg::Vec3Array(n);
        for (uint32_t i = 0; i < n; ++i)
            (*f->points)[i] = osg::Vec3(static_cast<float>(p[i].X()),
                                        static_cast<float>(p[i].Y()),
                                        static_cast<float>(p[i].Z()));
    }
    else if (fmt == InuDev::CPointCloudFrame::EFormat::e3DPointsRGB)
    {   // struct: float x,y,z; uint8_t r,g,b,a;
        const InuDev::CPointCloudFrame::C3DRGBPixel* p = frame->Get3DRGBData();
        f->points = new osg::Vec3Array(n); f->colors = new osg::Vec4ubArray(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            (*f->points)[i] = osg::Vec3(p[i].X(), p[i].Y(), p[i].Z());
            (*f->colors)[i] = osg::Vec4ub(p[i].R, p[i].G, p[i].B, p[i].Alpha);
        }
    }
    notifyPointCloud(f.get());
}

bool InuDevice::controlFeatures(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_featureStream) return true;
        _featureStream = _sensor->CreateFeaturesTrackingStream(_rgbChannel);
        if (!_featureStream) { OSG_NOTICE << "[InuDevice] Failed to create features stream\n"; return false; }
        ret = _featureStream->Init(InuDev::FeaturesTracking::EOutputType::eProcessed);
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init features: " << INUERR(ret) << "\n"; return false; }
        ret = _featureStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start features: " << INUERR(ret) << "\n"; return false; }
        ret = _featureStream->Register([this](auto s, auto f, auto e) { onFeaturesFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::Features, StreamType::Unknown);
    }
    else
    {
        if (_featureStream)
        {
            _featureStream->Register(nullptr); _featureStream->Stop();
            _featureStream->Terminate(); _featureStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::Features);
    }
    return true;
}

void InuDevice::onFeaturesFrame(std::shared_ptr<InuDev::CFeaturesTrackingStream>,
                                std::shared_ptr<const InuDev::CFeaturesTrackingFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<FeaturesFrame> f = new FeaturesFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    const int n = frame->GetKeyPointNumber();
    f->keypoints.resize(static_cast<size_t>(n));
    f->descriptorSize = frame->GetDescriptorSize();
    for (int i = 0; i < n; ++i)
    {
        const auto& kp = frame->GetProcessedData()[i];
        FeaturesFrame::FeaturePoint& p = f->keypoints[i];
        p.position.set(kp.X, kp.Y); p.id = static_cast<uint32_t>(kp.UniqId);
        if (kp.Descriptor && f->descriptorSize > 0)
            p.descriptor.assign(kp.Descriptor, kp.Descriptor + InuDev::FeaturesTracking::DESCR_SIZE);
    }
    notifyFeatures(f.get());
}

bool InuDevice::controlCNN(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        InuDev::CCnnAppFrame::EOutputType t = InuDev::CCnnAppFrame::eObjectDetection;
        if (_cnnMode.find("Segment") != std::string::npos) t = InuDev::CCnnAppFrame::eSegmentation;
        else if (_cnnMode.find("Class") != std::string::npos) t = InuDev::CCnnAppFrame::eClassification;
        else if (_cnnMode.find("Face") != std::string::npos) t = InuDev::CCnnAppFrame::eFaceRecognition;
        else if (_cnnMode.find("YoloV3") != std::string::npos) t = InuDev::CCnnAppFrame::eObjectDetectionYoloV3;
        else if (_cnnMode.find("YoloV7") != std::string::npos) t = InuDev::CCnnAppFrame::eObjectDetectionYoloV7;
        else if (_cnnMode.find("Pose") != std::string::npos) t = InuDev::CCnnAppFrame::ePoseDetection;
        else if (_cnnMode.find("Hand") != std::string::npos) t = InuDev::CCnnAppFrame::eHandDetection;

        _cnnStream = _sensor->CreateCnnAppStream(_rgbChannel);
        if (!_cnnStream) { OSG_NOTICE << "[InuDevice] Failed to create CNN stream\n"; return false; }
        ret = _cnnStream->Init(t);
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init CNN: " << INUERR(ret) << "\n"; return false; }
        ret = _cnnStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start CNN: " << INUERR(ret) << "\n"; return false; }
        ret = _cnnStream->Register([this](auto s, auto f, auto e) { onCNNFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::CNN, StreamType::Unknown);
    }
    else
    {
        if (_cnnStream)
        {
            _cnnStream->Register(nullptr); _cnnStream->Stop();
            _cnnStream->Terminate(); _cnnStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::CNN);
    }
    return true;
}

void InuDevice::onCNNFrame(std::shared_ptr<InuDev::CCnnAppStream>,
                           std::shared_ptr<const InuDev::CCnnAppFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<DetectionsFrame> f = new DetectionsFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    if (frame->GetOutputType() == InuDev::CCnnAppFrame::eObjectDetection ||
        frame->GetOutputType() == InuDev::CCnnAppFrame::eObjectDetectionYoloV3 ||
        frame->GetOutputType() == InuDev::CCnnAppFrame::eObjectDetectionYoloV7)
    {
        const std::vector<InuDev::CCnnAppFrame::CDetectedObject>& objs = *(frame->GetObjectData());
        for (const auto& o : objs)
        {
            DetectionsFrame::Detection d;
            d.label = o.ClassID; d.confidence = o.Confidence;
            d.bbox[0] = o.ClosedRectTopLeft.X(); d.bbox[1] = o.ClosedRectTopLeft.Y();
            d.bbox[2] = o.ClosedRectSize.X(); d.bbox[3] = o.ClosedRectSize.Y();
            f->detections.push_back(std::move(d));
        }
        f->detectionType = DetectionsFrame::ObjectDetect;
    }
    else if (frame->GetOutputType() == InuDev::CCnnAppFrame::eSegmentation)
    {
        const InuDev::CImageFrame& imgData = *(frame->GetSegmentationData());
        PixelFormat fmt = fromImageFormat(static_cast<InuDev::CImageStream::EOutputFormat>(imgData.Format()));
        f->segmentation = new osg::Image; wrapImage(f->segmentation.get(), &imgData, fmt);
        f->detectionType = DetectionsFrame::Segmentation;
    }
    else if (frame->GetOutputType() == InuDev::CCnnAppFrame::eClassification)
    {
        const InuDev::CCnnAppFrame::CClassificationData& clsData = *(frame->GetClassificationData());
        // TODO: what data to obtain?
        f->detectionType = DetectionsFrame::Classification;
    }
    else if (frame->GetOutputType() == InuDev::CCnnAppFrame::ePoseDetection)
    {
        const std::vector<InuDev::CCnnAppFrame::CDetectedPose>& objs = *(frame->GetPoseData());
        for (const auto& o : objs)
        {
            DetectionsFrame::Detection d;
            d.keypoints.resize(o.NumberOfKeyPoints);
            d.confidencesKP.resize(o.NumberOfKeyPoints);
            for (uint32_t i = 0; i < o.NumberOfKeyPoints; ++i)
            {
                d.keypoints[i] = osg::Vec2f(o.KeyPoints[i].P.X(), o.KeyPoints[i].P.Y());
                d.confidencesKP[i] = o.KeyPoints[i].Conf;
            }

            /*YoloV7Skeleton =
                std::make_pair(15, 13), std::make_pair(13, 11), std::make_pair(16, 14), std::make_pair(14, 12),
                std::make_pair(11, 12), std::make_pair(5, 11), std::make_pair(6, 12), std::make_pair(5, 6),
                std::make_pair(5, 7), std::make_pair(6, 8), std::make_pair(7, 9), std::make_pair(8, 10),
                std::make_pair(0, 1), std::make_pair(0, 2), std::make_pair(1, 3), std::make_pair(2, 4),
                std::make_pair(3, 5), std::make_pair(4, 6)*/
            f->detections.push_back(std::move(d));
        }
        f->detectionType = DetectionsFrame::PoseDetect;
    }
    else if (frame->GetOutputType() == InuDev::CCnnAppFrame::eHandDetection)
    {
        const std::vector<InuDev::CCnnAppFrame::CDetectedHand>& objs = *(frame->GetHandsData());
        for (const auto& o : objs)
        {
            DetectionsFrame::Detection d;
            d.keypoints.resize(o.HandPoints.size());
            for (uint32_t i = 0; i < o.HandPoints.size(); ++i)
                d.keypoints[i] = osg::Vec2f(o.HandPoints[i].X(), o.HandPoints[i].Y());

            d.label = (o.Side == InuDev::CCnnAppFrame::eRightHand) ? "_R" : "_L";
            switch (o.Gesture)
            {
            case InuDev::CCnnAppFrame::eOpenHand: d.label = "OpenHand" + d.label; break;
            case InuDev::CCnnAppFrame::eCloseHand: d.label = "CloseHand" + d.label; break;
            case InuDev::CCnnAppFrame::ePointRight: d.label = "PointRight" + d.label; break;
            case InuDev::CCnnAppFrame::ePointLeft: d.label = "PointLeft" + d.label; break;
            case InuDev::CCnnAppFrame::eThumbUp: d.label = "ThumbUp" + d.label; break;
            case InuDev::CCnnAppFrame::eOne: d.label = "One" + d.label; break;
            default: d.label = "None" + d.label; break;
            }

            /*HandConnectivitySkeleton =
                std::make_pair(0, 1), std::make_pair(1, 2), std::make_pair(2, 3), std::make_pair(3, 4),
                std::make_pair(0, 5), std::make_pair(5, 6), std::make_pair(6, 7), std::make_pair(7, 8),
                std::make_pair(5, 9), std::make_pair(9, 10), std::make_pair(10, 11), std::make_pair(11, 12),
                std::make_pair(9, 13), std::make_pair(13, 14), std::make_pair(14, 15), std::make_pair(15, 16),
                std::make_pair(13, 17), std::make_pair(17, 18), std::make_pair(18, 19),
                std::make_pair(19, 20), std::make_pair(0, 17)*/
            f->detections.push_back(std::move(d));
        }
        f->detectionType = DetectionsFrame::HandDetect;
    }
    else if (frame->GetOutputType() == InuDev::CCnnAppFrame::eFaceRecognition)
    {
        const std::vector<InuDev::CCnnAppFrame::CRecognizedFace>& objs = *(frame->GetFaceData());
        for (const auto& o : objs)
        {
            DetectionsFrame::Detection d;
            d.label = o.FaceID; d.confidence = o.Confidence;
            d.bbox[0] = o.ClosedRectTopLeft.X(); d.bbox[1] = o.ClosedRectTopLeft.Y();
            d.bbox[2] = o.ClosedRectSize.X(); d.bbox[3] = o.ClosedRectSize.Y();
            d.keypoints.resize(InuDev::CCnnAppFrame::CRecognizedFace::LANDMARKS_POINTS);
            for (uint32_t i = 0; i < d.keypoints.size(); ++i)
                d.keypoints[i] = osg::Vec2f(o.Landmarks[i].X(), o.Landmarks[i].Y());
            f->detections.push_back(std::move(d));
        }
        f->detectionType = DetectionsFrame::FaceDetect;
    }
    f->width = frame->GetWidth(); f->height = frame->GetHeight();
    notifyDetections(f.get());
}

bool InuDevice::controlTemperature(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        if (_tempStream) return true;
        _tempStream = _sensor->CreateTemperaturesStream(InuDev::CTemperaturesFrame::eAll);
        if (!_tempStream) { OSG_NOTICE << "[InuDevice] Failed to create temperature stream\n"; return false; }
        ret = _tempStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init temperature: " << INUERR(ret) << "\n"; return false; }
        ret = _tempStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start temperature: " << INUERR(ret) << "\n"; return false; }
        ret = _tempStream->Register([this](auto s, auto f, auto e) { onTemperatureFrame(s, f, e); });
        if (ret != InuDev::eOK) return false;
        updateActiveStreams(StreamType::Temperature, StreamType::Unknown);
    }
    else
    {
        if (_tempStream)
        {
            _tempStream->Register(nullptr); _tempStream->Stop();
            _tempStream->Terminate(); _tempStream.reset();
        }
        updateActiveStreams(StreamType::Unknown, StreamType::Temperature);
    }
    return true;
}

void InuDevice::onTemperatureFrame(std::shared_ptr<InuDev::CTemperaturesStream>,
                                   std::shared_ptr<const InuDev::CTemperaturesFrame> frame, InuDev::CInuError err)
{
    osg::ref_ptr<TemperatureFrame> f = new TemperatureFrame;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    else { f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex; }

    f->temperatures.resize(3);
    frame->GetTemperature(InuDev::CTemperaturesFrame::eSensorLeft, f->temperatures[0]);
    frame->GetTemperature(InuDev::CTemperaturesFrame::eSensorRight, f->temperatures[1]);
    frame->GetTemperature(InuDev::CTemperaturesFrame::ePVT, f->temperatures[2]);
    notifyTemperature(f.get());
}
