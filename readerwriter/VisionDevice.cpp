#include <cstdint>
#include <type_traits>
#include <limits>
#if defined(_MSC_VER)
#  include <intrin.h>
#  pragma intrinsic(_BitScanForward, _BitScanForward64)
#endif
#include "VisionDevice.h"
using namespace osgVerse;

namespace
{
    static constexpr size_t kMaxQueue = 4;

#if defined(_MSC_VER)
    template<typename T> typename std::enable_if<sizeof(T) == 8, unsigned int>::type ctz_impl(T value)
    {
        unsigned long index = 0; _BitScanForward64(&index, static_cast<unsigned __int64>(value));
        return static_cast<unsigned int>(index);
    }

    template<typename T> typename std::enable_if<sizeof(T) == 4, unsigned int>::type ctz_impl(T value)
    {
        unsigned long index = 0; _BitScanForward(&index, static_cast<unsigned long>(value));
        return static_cast<unsigned int>(index);
    }

    template<typename T> typename std::enable_if<sizeof(T) == 2, unsigned int>::type ctz_impl(T value)
    {
        unsigned long index = 0; _BitScanForward(&index, static_cast<unsigned long>(value));
        return static_cast<unsigned int>(index);
    }

    template<typename T> typename std::enable_if<sizeof(T) == 1, unsigned int>::type ctz_impl(T value)
    {
        unsigned long index = 0; _BitScanForward(&index, static_cast<unsigned long>(value));
        return static_cast<unsigned int>(index);
    }
#elif defined(__GNUC__) || defined(__clang__)
    template<typename T> typename std::enable_if<sizeof(T) == 8, unsigned int>::type ctz_impl(T value)
    { return static_cast<unsigned int>(__builtin_ctzll(value)); }

    template<typename T> typename std::enable_if<sizeof(T) == 4, unsigned int>::type ctz_impl(T value)
    { return static_cast<unsigned int>(__builtin_ctz(value)); }

    template<typename T> typename std::enable_if<sizeof(T) == 2, unsigned int>::type ctz_impl(T value)
    { return static_cast<unsigned int>(__builtin_ctz(static_cast<unsigned int>(value))); }

    template<typename T> typename std::enable_if<sizeof(T) == 1, unsigned int>::type ctz_impl(T value)
    { return static_cast<unsigned int>(__builtin_ctz(static_cast<unsigned int>(value))); }
#else
    template<typename T> unsigned int ctz_impl(T value) noexcept
    {
        const unsigned int bits = static_cast<unsigned int>(sizeof(T) * 8);
        for (unsigned int i = 0; i < bits; ++i) { if (value & (static_cast<T>(1) << i)) return i; }
        return bits;
    }
#endif

    template<typename T> inline unsigned int ctz(T value)
    {
        if (value == 0)
            return static_cast<unsigned int>(sizeof(T) * 8);
        return ctz_impl<T>(value);
    }

    inline int bitIndex(VisionInputDevice::StreamType t)
    {
        unsigned int v = static_cast<unsigned int>(t);
        if (v == 0u) return 0; return ctz(v);
    }
}

VisionInputDevice::VisionInputDevice()
{
    setState(DeviceState::Disconnected); setActiveStreams(StreamType::Unknown);
    setCapabilities(osgGA::Device::SEND_EVENTS);
}

VisionInputDevice::VisionInputDevice(const VisionInputDevice& copy, const osg::CopyOp& copyop)
:   osgGA::Device(copy, copyop)
{}

VisionInputDevice::~VisionInputDevice()
{}

void VisionInputDevice::sendEvent(const osgGA::Event& ev)
{
    // TODO
    OSG_NOTICE << "[VisionInputDevice] Not implemented...\n";
}

bool VisionInputDevice::stopAllStreams()
{ return stopStream(static_cast<StreamType>(_activeMask.load(std::memory_order_acquire))); }

unsigned int VisionInputDevice::getFrameCount(StreamType type) const
{
    if (static_cast<unsigned int>(type) == 0u) return 0u;
    unsigned int total = 0, v = static_cast<unsigned int>(type);
    while (v)
    {
        int idx = ctz(v);
        total += _stats[idx].frameCount.load(std::memory_order_relaxed); v &= v - 1u;
    }
    return total;
}

unsigned int VisionInputDevice::getErrorCount(StreamType type) const
{
    if (static_cast<unsigned int>(type) == 0u) return 0u;
    unsigned int total = 0, v = static_cast<unsigned int>(type);
    while (v)
    {
        int idx = ctz(v);
        total += _stats[idx].errorCount.load(std::memory_order_relaxed); v &= v - 1u;
    }
    return total;
}

bool VisionInputDevice::isStreaming(StreamType mask) const
{
    if (static_cast<unsigned int>(mask) == 0u) return false;
    unsigned int active = _activeMask.load(std::memory_order_acquire);
    return (active & static_cast<unsigned int>(mask)) != 0u;
}

VisionInputDevice::DeviceState VisionInputDevice::getState() const
{ return _state.load(std::memory_order_acquire); }

void VisionInputDevice::setState(DeviceState s)
{ _state.store(s, std::memory_order_release); }

void VisionInputDevice::setActiveStreams(StreamType mask)
{ _activeMask.store(static_cast<unsigned int>(mask), std::memory_order_release); }

VisionInputDevice::StreamType VisionInputDevice::updateActiveStreams(StreamType add, StreamType remove)
{
    unsigned int expected = _activeMask.load(std::memory_order_acquire), desired = 0;
    do { desired = (expected | static_cast<unsigned int>(add)) & ~static_cast<unsigned int>(remove); }
    while (!_activeMask.compare_exchange_weak(expected, desired, std::memory_order_acq_rel));
    return static_cast<StreamType>(expected);
}

void VisionInputDevice::reportError(StreamType type)
{
    unsigned int v = static_cast<unsigned int>(type);
    while (v)
    {
        int idx = ctz(v);
        _stats[idx].errorCount.fetch_add(1, std::memory_order_relaxed); v &= v - 1u;
    }
}

void VisionInputDevice::resetStats()
{
    for (auto& s : _stats)
    {
        s.frameCount.store(0, std::memory_order_relaxed);
        s.errorCount.store(0, std::memory_order_relaxed);
    }
}

void VisionInputDevice::notifyImage(StreamType type, ImageFrame* f)
{
    if (static_cast<unsigned int>(type) == 0u) return;
    int idx = bitIndex(type);
    {
        std::lock_guard<std::mutex> lk(_imgMtx[idx]); _imgQueue[idx].push_back(f);
        while (_imgQueue[idx].size() > kMaxQueue) _imgQueue[idx].pop_front();
    }
    _stats[idx].frameCount.fetch_add(1, std::memory_order_relaxed);
    _imgCV[idx].notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(type);
    ev->image = f; sendEvent(*ev);
}

void VisionInputDevice::notifyStereoImage(StreamType type, StereoImageFrame* f)
{
    if (static_cast<unsigned int>(type) == 0u) return;
    int idx = bitIndex(type);
    {
        std::lock_guard<std::mutex> lk(_stereoMtx[idx]); _stereoQueue[idx].push_back(f);
        while (_stereoQueue[idx].size() > kMaxQueue) _stereoQueue[idx].pop_front();
    }
    _stats[idx].frameCount.fetch_add(1, std::memory_order_relaxed);
    _stereoCV[idx].notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(type);
    ev->stereo = f; sendEvent(*ev);
}

void VisionInputDevice::notifyIMU(IMUSampleFrame* s)
{
    {
        std::lock_guard<std::mutex> lk(_imuMtx); _imuQueue.push_back(s);
        while (_imuQueue.size() > kMaxQueue) _imuQueue.pop_front();
    }
    _stats[bitIndex(StreamType::IMU)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _imuCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::IMU);
    ev->imu = s; sendEvent(*ev);
}

void VisionInputDevice::notifyPose(PoseFrame* f)
{
    {
        std::lock_guard<std::mutex> lk(_poseMtx); _poseQueue.push_back(f);
        while (_poseQueue.size() > kMaxQueue) _poseQueue.pop_front();
    }
    _stats[bitIndex(StreamType::SLAM)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _poseCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::SLAM);
    ev->pose = f; sendEvent(*ev);
}

void VisionInputDevice::notifyPointCloud(PointCloudFrame* f)
{
    {
        std::lock_guard<std::mutex> lk(_cloudMtx); _cloudQueue.push_back(f);
        while (_cloudQueue.size() > kMaxQueue) _cloudQueue.pop_front();
    }
    _stats[bitIndex(StreamType::PointCloud)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _cloudCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::PointCloud);
    ev->cloud = f; sendEvent(*ev);
}

void VisionInputDevice::notifyDetections(DetectionsFrame* f)
{
    {
        std::lock_guard<std::mutex> lk(_detMtx); _detQueue.push_back(f);
        while (_detQueue.size() > kMaxQueue) _detQueue.pop_front();
    }
    _stats[bitIndex(StreamType::CNN)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _detCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::CNN);
    ev->detections = f; sendEvent(*ev);
}

void VisionInputDevice::notifyFeatures(FeaturesFrame* f)
{
    {
        std::lock_guard<std::mutex> lk(_featMtx); _featQueue.push_back(f);
        while (_featQueue.size() > kMaxQueue) _featQueue.pop_front();
    }
    _stats[bitIndex(StreamType::Features)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _featCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::Features);
    ev->features = f; sendEvent(*ev);
}

void VisionInputDevice::notifyTemperature(TemperatureFrame* f)
{
    {
        std::lock_guard<std::mutex> lk(_tempMtx); _tempQueue.push_back(f);
        while (_tempQueue.size() > kMaxQueue) _tempQueue.pop_front();
    }
    _stats[bitIndex(StreamType::Temperature)].frameCount.fetch_add(1, std::memory_order_relaxed);
    _tempCV.notify_all();

    osg::ref_ptr<StreamEvent> ev = new StreamEvent(StreamType::Temperature);
    ev->temperature = f; sendEvent(*ev);
}
