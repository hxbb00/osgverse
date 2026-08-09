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
        bool getDecodedFrame(uint8_t** frameData, int* pitch, long long* pts);
        void releaseFrame();

        struct ExportedBufferData
        {
            int fd, drmPrimePitch, drmPrimeOffset;
            uint64_t drmPrimeModifier;
        };
        bool exportDecodedFrame(std::vector<ExportedBufferData>& bufferData);
        
        int getWidth() const { return _width; }
        int getHeight() const { return _height; }
        int getBitDepth() const { return _bitDepth; }
        
        struct gbm_bo* getGBMBuffer() { return _gbmBuffer; }
        uint32_t getFramebufferID() const { return _framebufferID; }
        void* getEglImage() const { return _eglImage; }

    private:
        bool setupVAAPI();
        bool setupGBM();
        bool createSurface();
        bool createBuffer();
        bool syncSurface();
        void cleanupVAAPI();
        void cleanupGBM();

        VADisplay _vaDisplay;
        VAConfigID _vaConfig;
        VAContextID _vaContext;
        VASurfaceID _vaSurface;
        VABufferID _vaBuffer;
        
        struct gbm_device* _gbmDevice;
        struct gbm_surface* _gbmSurface;
        struct gbm_bo* _gbmBuffer;
        void* _eglImage;
        std::mutex _mutex;
        
        long long _currentPTS;
        unsigned int _framebufferID;
        int _videoCodec, _drmFD;
        int _width, _height, _bitDepth;
        bool _initialized, _frameReady;
    };

}
