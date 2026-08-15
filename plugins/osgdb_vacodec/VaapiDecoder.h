#pragma once
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <gbm.h>
#include <vector>
#include <mutex>
#include <osg/Referenced>

namespace osgVerse
{
    
    class VaapiDecoder : public osg::Referenced
    {
    public:
        VaapiDecoder(int videoCodec, int width, int height);
        virtual ~VaapiDecoder();

        bool initialize();
        bool decode(const uint8_t* data, int size, long long pts);
        bool convert();
        void releaseFrame();

        struct ExportedBufferData
        {
            int fd, drmPrimePitch, drmPrimeOffset;
            uint64_t drmPrimeModifier;
        };
        bool exportDecodedFrame(int& fourcc, std::vector<ExportedBufferData>& bufferData);
        
        int getWidth() const { return _width; }
        int getHeight() const { return _height; }
        int getBitDepth() const { return _bitDepth; }
        uint32_t getFramebufferID() const { return _framebufferID; }
        bool isInitialized() const { return _initialized; }

    private:
        bool setupVAAPI();
        bool setupGBM();
        bool createSurface();
        bool createBuffer();
        bool syncSurface(bool origin);
        void cleanupVAAPI();
        void cleanupGBM();

        VADisplay _vaDisplay;
        VAConfigID _vaConfig;
        VAConfigID _vppConfig;
        VAContextID _vaContext;
        VAContextID _vppContext;
        VASurfaceID _vaSurface;
        VASurfaceID _vaSurfaceRGB; 
        
        struct gbm_device* _gbmDevice;
        struct gbm_surface* _gbmSurface;
        struct gbm_bo* _gbmBuffer;
        std::mutex _mutex;
        
        long long _currentPTS;
        unsigned int _framebufferID;
        int _videoCodec, _drmFD;
        int _width, _height, _bitDepth;
        bool _initialized, _frameReady;
    };

}
