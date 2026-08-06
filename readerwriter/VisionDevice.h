#ifndef MANA_READERWRITER_VISIONDEVICE_HPP
#define MANA_READERWRITER_VISIONDEVICE_HPP

#include <osg/Texture>
#include <osgDB/ReaderWriter>
#include <osgGA/Device>
#include <sstream>
#include <iostream>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <map>
#include "Export.h"

namespace osgVerse
{
    /// Frame definition base
    struct VisionFrameBase : public osg::Referenced
    {
        unsigned long long timestamp = 0;     ///< nanoseconds
        unsigned int frameIndex = 0;
        VisionFrameBase() = default;
        virtual ~VisionFrameBase() = default;
    };

    /// Generic image frame
    struct ImageFrame : public VisionFrameBase
    {
        osg::ref_ptr<osg::Image> image;
        ImageFrame() = default;
    };

    /// Stereo pair (used for IR streams)
    struct StereoImageFrame : public VisionFrameBase
    {
        ImageFrame left, right;
        StereoImageFrame() = default;
    };

    /// A single IMU sample
    struct IMUSampleFrame : public VisionFrameBase
    {
        osg::Vec3d gyro;            ///< rad/s
        osg::Vec3d accel;           ///< m/s^2
        osg::Vec3d magnetometer;    ///< optional, gauss
        bool hasGyro = false, hasAccel = false, hasMag = false;
        IMUSampleFrame() = default;
    };

    /// 6-DoF pose frame (typically from SLAM/tracking)
    struct PoseFrame : public VisionFrameBase
    {
        enum TrackingState
        {
            NotSupported = 0, Tracking = 1 << 0, LostNoReloc = 1 << 1,
            LostFewFeatures = 1 << 2, LostFewStereoPairs = 1 << 3, LostFewMatchers = 1 << 4,
            LostFewInliers1 = 1 << 5, LostFewInliers2 = 1 << 6, LostGenerally = 1 << 7,
            Relocalizated = 1 << 8, LoopClosed = 1 << 9, OnlyIMU = 1 << 10
        };

        osg::Quat orientation;          ///< body -> world
        osg::Vec3d translation;         ///< metres
        unsigned int state = 0;         ///< tracking state
        PoseFrame() = default;
    };

    /// Point cloud frame (organised or unorganised)
    struct PointCloudFrame : public VisionFrameBase
    {
        osg::ref_ptr<osg::Vec3Array>   points;
        osg::ref_ptr<osg::Vec4ubArray> colors;   ///< optional, may be null
        unsigned int width = 0, height = 0;      ///< H=1 for unorganised clouds
        PointCloudFrame() = default;
    };

    /// A full set of NN inference results for one frame
    struct DetectionsFrame : public VisionFrameBase
    {
        struct Detection
        {
            std::string label;
            float confidence = 0.f;
            float bbox[4] = {0.f, 0.f, 0.f, 0.f};  /// Axis-aligned (x, y, w, h)
            std::vector<osg::Vec2f> keypoints;     /// Optional 2D keypoints (pose / hand / face)
            std::vector<float> confidencesKP;      /// Optional 2D keypoint confidences
            Detection() = default;
        };

        enum DetectionType
        {
            ObjectDetect = 0x1, Segmentation = 0x2, Classification = 0x4,
            PoseDetect = 0x8, HandDetect = 0x10, FaceDetect = 0x20
        };
        unsigned int detectionType = 0, width = 0, height = 0;

        std::vector<Detection> detections;            /// Detections or classifications
        osg::ref_ptr<osg::Image> segmentation;        /// Optional segmentation mask
        DetectionsFrame() = default;
    };

    /// Features tracking frame
    struct FeaturesFrame : public VisionFrameBase
    {
        struct FeaturePoint
        {
            osg::Vec2f position;
            unsigned int id = 0;
            std::vector<unsigned int> descriptor;   ///< optional 512 bits FREAK/LIFT binary descriptor
            FeaturePoint() = default;
        };
        std::vector<FeaturePoint> keypoints;

        unsigned int descriptorSize = 0;       ///< bytes per descriptor, 0 if none
        FeaturesFrame() = default;
    };

    /// Temperature report from one or more on-board sensors
    struct TemperatureFrame : public VisionFrameBase
    {
        std::vector<float> temperatures;   ///< degrees Celsius, per sensor
        TemperatureFrame() = default;
    };

    /// Aggregated calibration for an RGBD device
    struct Calibration
    {
        struct CameraIntrinsics
        {
            float fx = 0.f, fy = 0.f;
            float cx = 0.f, cy = 0.f;
            std::vector<double> distortion;  ///< vendor-specific ordering
            CameraIntrinsics() = default;
        };

        struct CameraExtrinsics
        {
            float rotation[3] = {0.f, 0.f, 0.f};
            float translation[3] = {0.f, 0.f, 0.f};
            CameraExtrinsics() = default;
        };

        CameraIntrinsics depthIntrinsics;
        CameraIntrinsics rgbIntrinsics;
        CameraExtrinsics depthToRgb;
        float baseline = 0.f;   ///< metres
        bool depthValid = false, rgbValid = false, dToRgbValid = false;
        Calibration() = default;
    };

    class OSGVERSE_RW_EXPORT VisionInputDevice : public osgGA::Device
    {
    public:
        enum class StreamType
        {
            Unknown      = 0,
            RGB          = 1 << 0,
            Depth        = 1 << 1,
            IR           = 1 << 2,   ///< IR stereo pair (left/right)
            Fisheye      = 1 << 3,
            PointCloud   = 1 << 4,
            IMU          = 1 << 5,
            SLAM         = 1 << 6,   ///< 6-DoF pose stream
            Features     = 1 << 7,   ///< Features tracking
            Temperature  = 1 << 8,
            CNN          = 1 << 9,   ///< On-chip NN inference results
            All          = 0x0FFFFFFF
        };

        enum class PixelFormat
        {
            Unknown,
            Raw,        ///< Sensor-specific raw bytes
            Gray8,      ///< 8-bit monochrome
            BGR8,       ///< 24-bit BGR
            RGB8,       ///< 24-bit RGB
            BGRA8,      ///< 32-bit BGRA
            RGBA8,      ///< 32-bit RGBA
            Depth16,    ///< 16-bit unsigned depth in millimetres
            Depth32F    ///< 32-bit float depth in metres
        };

        enum class DeviceState
        { Disconnected, Connected, Streaming, Error };

        struct StreamEvent : public osgGA::Event
        {
            osg::ref_ptr<ImageFrame> image;
            osg::ref_ptr<StereoImageFrame> stereo;
            osg::ref_ptr<IMUSampleFrame> imu;
            osg::ref_ptr<PoseFrame> pose;
            osg::ref_ptr<PointCloudFrame> cloud;
            osg::ref_ptr<DetectionsFrame> detections;
            osg::ref_ptr<FeaturesFrame> features;
            osg::ref_ptr<TemperatureFrame> temperature;
            StreamType type;

            StreamEvent() : osgGA::Event(), type(StreamType::Unknown) {}
            StreamEvent(StreamType t) : osgGA::Event(), type(t) {}
        };
        
        VisionInputDevice();
        VisionInputDevice(const VisionInputDevice&, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
        virtual void sendEvent(const osgGA::Event& ev);

        typedef std::function<void (StreamEvent*)> EventCallback;
        void setEventCallback(EventCallback cb) { _eventCallback = cb; }
        EventCallback getEventCallback() { return _eventCallback; }

        virtual const char* getDeviceClassName() const = 0;
        virtual const char* getDeviceModelName() const = 0;
        virtual std::string getSerialNumber() const = 0;
        virtual std::string getFirmwareVersion() const = 0;

        virtual bool connect(const osgDB::Options* opts) = 0;
        virtual bool disconnect() = 0;

        virtual bool configure(const osgDB::Options* opts) = 0;
        virtual bool getCalibration(Calibration& out) const = 0;
        virtual bool startStream(StreamType mask) = 0;
        virtual bool stopStream(StreamType mask) = 0;
        bool stopAllStreams();

        virtual bool supports(StreamType type) const = 0;
        bool isStreaming(StreamType mask) const;
        DeviceState getState() const;

        // Number of frames delivered for \p type since connect()
        virtual unsigned int getFrameCount(StreamType type) const;

        /// Number of errors observed on \p type since connect()
        virtual unsigned int getErrorCount(StreamType type) const;

    protected:
        virtual ~VisionInputDevice();
        void setState(DeviceState s);  /// Called by subclasses to update their reported state
        void setActiveStreams(StreamType mask);  /// Called by subclasses to mark which streams are currently active
        StreamType updateActiveStreams(StreamType add, StreamType remove);  /// Called by subclasses to start/stop streams
        
        void reportError(StreamType type);  /// Increment the per-stream error counter
        void resetStats();  /// Reset all per-stream statistics (e.g. on connect)

        // frame-arrival helpers (subclasses call these from SDK callbacks) --
        void notifyImage      (StreamType type, ImageFrame* f);
        void notifyStereoImage(StreamType type, StereoImageFrame* f);
        void notifyIMU        (IMUSampleFrame* s);
        void notifyPose       (PoseFrame* f);
        void notifyPointCloud (PointCloudFrame* f);
        void notifyDetections (DetectionsFrame* f);
        void notifyFeatures   (FeaturesFrame* f);
        void notifyTemperature(TemperatureFrame* f);

        struct PerStreamStats
        {
            std::atomic<unsigned int> frameCount{0};
            std::atomic<unsigned int> errorCount{0};
        };
        PerStreamStats _stats[32];   // indexed by bit position of StreamType
        std::atomic<DeviceState> _state;
        std::atomic<unsigned int> _activeMask;
        EventCallback _eventCallback;

        // ring-buffers (kept tiny - latest-N) per stream kind
        std::deque<osg::ref_ptr<ImageFrame>> _imgQueue[32];
        std::deque<osg::ref_ptr<StereoImageFrame>> _stereoQueue[32];
        std::deque<osg::ref_ptr<IMUSampleFrame>> _imuQueue;
        std::deque<osg::ref_ptr<PoseFrame>> _poseQueue;
        std::deque<osg::ref_ptr<PointCloudFrame>> _cloudQueue;
        std::deque<osg::ref_ptr<DetectionsFrame>> _detQueue;
        std::deque<osg::ref_ptr<FeaturesFrame>> _featQueue;
        std::deque<osg::ref_ptr<TemperatureFrame>> _tempQueue;
        mutable std::mutex _imgMtx[32], _stereoMtx[32];
        mutable std::mutex _imuMtx, _poseMtx, _cloudMtx;
        mutable std::mutex _detMtx, _featMtx, _tempMtx;
        std::condition_variable _imgCV[32], _stereoCV[32];
        std::condition_variable _imuCV, _poseCV, _cloudCV;
        std::condition_variable _detCV, _featCV, _tempCV;
    };
}

#endif
