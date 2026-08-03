#include "InuDevice.h"
using namespace osgVerse;

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
        case VisionInputDevice::PixelFormat::Depth16:  return GL_R16UI;
        case VisionInputDevice::PixelFormat::Depth32F: return GL_R32F;
        default:                                       return GL_R8;
        }
    }
    GLenum toPixelFormat(VisionInputDevice::PixelFormat pf)
    {
        switch (pf)
        {
            case VisionInputDevice::PixelFormat::Gray8:    return GL_LUMINANCE;
            case VisionInputDevice::PixelFormat::BGR8:     return GL_BGR;
            case VisionInputDevice::PixelFormat::RGB8:     return GL_RGB;
            case VisionInputDevice::PixelFormat::BGRA8:    return GL_BGRA;
            case VisionInputDevice::PixelFormat::RGBA8:    return GL_RGBA;
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
        default:                          return VisionInputDevice::PixelFormat::BGR8;
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
:   _fps(10), _binning(0), _rgbChannel(4), _irChannel(0), _depthChannel(3), _fisheyeChannel(2),
    _rgbRegistrationChannel(-1), _depthRegistrationChannel(-1), _calibrationLoaded(false)
{
    _rgbCache = new ImageFrame; _depthCache = new ImageFrame;
}

InuDevice::~InuDevice()
{
    stopAllStreams();
    if (getState() != DeviceState::Disconnected)
        disconnect();
}

bool InuDevice::configure(const osgDB::Options* opts)
{
    if (!_sensor) return false;
    if (!opts) return true;

    std::string cfg = opts->getPluginStringData("ConfigEnabledRGB");
    if (atoi(cfg.c_str()) > 0)
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

        // RGB sensor index = 2
        InuDev::CInuError ret = _sensor->SetSensorControlParams(p, 2);
        if (ret != InuDev::eOK)
        {
            OSG_NOTICE << "[InuDevice] RGB SetSensorControlParams failed: 0x"
                       << std::hex << int(ret) << std::endl; return false;
        }
    }

    cfg = opts->getPluginStringData("ConfigEnabledIR");
    if (atoi(cfg.c_str()) > 0)
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

        InuDev::CInuError ret = _sensor->SetSensorControlParams(p, 0);
        if (ret != InuDev::eOK)
        {
            OSG_NOTICE << "[InuDevice] IR SetSensorControlParams failed: 0x"
                       << std::hex << int(ret) << std::endl; return false;
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

    // RGB sensor index 2
    InuDev::CCalibrationData rgbCalib;
    if (_sensor) _sensor->GetCalibrationData(rgbCalib, 4);
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
        std::string rgbCh = opts->getPluginStringData("ChannelRGB");
        std::string depthCh = opts->getPluginStringData("ChannelDepth");
        _deviceId = opts->getPluginStringData("DeviceID");
        _ipAddress = opts->getPluginStringData("IpAddress");

        if (!fpsV.empty()) _fps = atoi(fpsV.c_str());
        if (!binV.empty()) _binning = atoi(binV.c_str());
        if (!rgbCh.empty()) _rgbRegistrationChannel = atoi(rgbCh.c_str());
        if (!depthCh.empty()) _depthRegistrationChannel = atoi(depthCh.c_str());
    }

    if (!createSensor()) { setState(DeviceState::Error); return false; }
    if (!startSensor()) { setState(DeviceState::Error); return false; }
    setState(DeviceState::Initialized); return true;
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

    if (isStreaming(StreamType::All))
        setState(DeviceState::Streaming);
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

    if (isStreaming(StreamType::All))
        setState(DeviceState::Initialized);
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
                   << int(ret) << " - " << std::string(ret) << std::endl;
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
                   << int(ret) << " - " << std::string(ret) << std::endl;
        return false;
    }

    std::map<InuDev::CEntityVersion::EEntitiesID, InuDev::CEntityVersion> versions;
    ret = _sensor->GetVersion(versions);
    if (ret == InuDev::eOK)
    {
        _serialNum = versions[InuDev::CEntityVersion::eSerialNumber].VersionName;
        _firmwareVersion = versions[InuDev::CEntityVersion::eFWVersion].VersionName;
        _calibVersion = versions[InuDev::CEntityVersion::eCalibrationVersion].VersionName;
        //_bootfixVersion = versions[InuDev::CEntityVersion::eBootfixVersion].VersionName;
        //_usbType = versions[InuDev::CEntityVersion::eUSBSpeed].VersionNum;

        // Pick a friendly model name from the SN prefix
        if (_serialNum.find("BBA") != std::string::npos) _modelName = "R132";
        else if (_serialNum.find("CAA") != std::string::npos) _modelName = "R130";
        else if (_serialNum.find("ABA") != std::string::npos ||
                 _serialNum.find("JHT") != std::string::npos) _modelName = "C158";
        else if (_serialNum.find("ABB") != std::string::npos) _modelName = "C158Tracking";
        else _modelName = "UnknownModel";
    }

    InuDev::ESensorResolution depthRes = toResolution(_binning);
    InuDev::ESensorResolution rgbRes = toResolution(_binning);
    InuDev::CStartDeviceParamsExt startParams;
    startParams.VecDpeParams = dpeStart;
    for (const auto& ch : hwInfo.GetChannels())
    {
        const uint32_t id = ch.second.ChannelId;
        switch (ch.second.ChannelType)
        {
        case InuDev::eDepthChannel: case InuDev::eDisparityChannel:
            startParams.ChannelControlParam[id].SensorRes = depthRes;
            startParams.ChannelControlParam[id].FPS = _fps;
            if (_depthRegistrationChannel != -1)
            {
                startParams.ChannelControlParam[id].ActivateRegisteredDepth = true;
                startParams.ChannelControlParam[id].RegisteredDepthChannelID =
                    static_cast<uint32_t>(_depthRegistrationChannel);
            }
            break;
        case InuDev::eGeneralCameraChannel:
            startParams.ChannelControlParam[id].SensorRes = rgbRes;
            startParams.ChannelControlParam[id].FPS = _fps;
            break;
        case InuDev::eTrackingChannel:
            startParams.ChannelControlParam[id].SensorRes = InuDev::eFull;
            startParams.ChannelControlParam[id].FPS = 30;
            startParams.ChannelControlParam[id].InterleaveMode = InuDev::eInterleave;
            break;
        case InuDev::eStereoChannel:
            startParams.ChannelControlParam[id].SensorRes = toResolution(_binning);
            startParams.ChannelControlParam[id].FPS = _fps;
            startParams.ChannelControlParam[id].InterleaveMode = InuDev::eInterleave;
            break;
        default: break;
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
                   << " - " << std::string(ret) << std::endl;
    }

    ret = _sensor->Terminate();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Terminate failed: 0x" << std::hex << int(ret)
                   << " - " << std::string(ret) << std::endl;
    }

    ret = _sensor->Disconnect();
    if (ret != InuDev::eOK)
    {
        OSG_NOTICE << "[InuDevice] Disconnect failed: 0x" << std::hex << int(ret)
                   << " - " << std::string(ret) << std::endl;
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
            _rgbRegStream = _sensor->CreateImageRegisteredStream(static_cast<uint32_t>(_rgbChannel), regCh);
            if (!_rgbRegStream) { OSG_NOTICE << "[InuDevice] Failed to create image stream"; return false; }
            ret = _rgbRegStream->Init(outFmt, ppe, InuDev::CDepthProperties::EPostProcessing::eDefaultPP);
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init image stream"; return false; }
            ret = _rgbRegStream->Start();
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start image stream"; return false; }
            ret = _rgbRegStream->Register([this](auto s, auto f, auto e) { onRGBFrame(s, f, e); });
        }
        else
        {
            _rgbStream = _sensor->CreateImageStream(static_cast<uint32_t>(_rgbChannel));
            if (!_rgbStream) { OSG_NOTICE << "[InuDevice] Failed to create image stream"; return false; }
            ret = _rgbStream->Init(outFmt, ppe);
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init image stream"; return false; }
            ret = _rgbStream->Start();
            if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start image stream"; return false; }
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
    osg::ref_ptr<ImageFrame> f = _rgbCache;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    PixelFormat fmt = fromImageFormat(static_cast<InuDev::CImageStream::EOutputFormat>(frame->Format()));
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    if (!f->image) f->image = new osg::Image;
    wrapImage(f->image.get(), frame.get(), fmt);
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
        
        _depthStream = _sensor->CreateDepthStream(static_cast<uint32_t>(_depthChannel));
        if (!_depthStream) { OSG_NOTICE << "[InuDevice] Failed to create depth stream"; return false; }
        ret = _depthStream->Init();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to init depth stream"; return false; }
        ret = _depthStream->Start();
        if (ret != InuDev::eOK) { OSG_NOTICE << "[InuDevice] Failed to start depth stream"; return false; }

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
    osg::ref_ptr<ImageFrame> f = _depthCache;
    if (err != InuDev::eOK || !frame || !frame->Valid) return;
    
    f->timestamp = frame->Timestamp; f->frameIndex = frame->FrameIndex;
    if (!f->image) f->image = new osg::Image;
    wrapImage(f->image.get(), frame.get(), PixelFormat::Depth16);
    notifyImage(StreamType::Depth, f.get());
}

bool InuDevice::controlIR(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlFisheye(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlIMU(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlSLAM(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlPointCloud(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlFeatures(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlCNN(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}

bool InuDevice::controlTemperature(bool started)
{
    InuDev::CInuError ret = InuDev::eOK;
    if (started)
    {
        // TODO
    }
    else
    {
        // TODO
    }
    return true;
}
