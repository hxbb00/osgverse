#include <osg/Notify>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include "VaapiDecoder.h"
using namespace osgVerse;

VaapiDecoder::VaapiDecoder(int videoCodec, int width, int height)
:   _vaDisplay(nullptr), _vaConfig(VA_INVALID_ID), _vaContext(VA_INVALID_ID),
    _vaSurface(VA_INVALID_SURFACE), _vaBuffer(VA_INVALID_ID), _gbmDevice(nullptr),
    _gbmSurface(nullptr), _gbmBuffer(nullptr), _eglImage(nullptr), _currentPTS(0),
    _framebufferID(0), _videoCodec(videoCodec), _drmFD(-1), _width(width), _height(height),
    _bitDepth(8), _initialized(false), _frameReady(false)
{}

VaapiDecoder::~VaapiDecoder()
{
    cleanupVAAPI();
    cleanupGBM();
}

bool VaapiDecoder::initialize()
{
    if (_initialized) return true;
    if (!setupGBM())
    {
        OSG_WARN << "[VaapiDecoder] Failed to setup GBM" << std::endl;
        return false;
    }
    
    if (!setupVAAPI())
    {
        OSG_WARN << "[VaapiDecoder] Failed to setup VA-API" << std::endl;
        cleanupGBM(); return false;
    }
    
    if (!createSurface())
    {
        OSG_WARN << "[VaapiDecoder] Failed to create surface" << std::endl;
        cleanupVAAPI(); cleanupGBM(); return false;
    }
    _initialized = true; return true;
}

bool VaapiDecoder::setupGBM()
{
    _drmFD = open("/dev/dri/renderD128", O_RDWR);
    if (_drmFD < 0)
    {
        _drmFD = open("/dev/dri/card0", O_RDWR);
        if (_drmFD < 0)
        {
            OSG_WARN << "[VaapiDecoder] Failed to open DRM device" << std::endl;
            return false;
        }
    }
    
    _gbmDevice = gbm_create_device(_drmFD);
    if (!_gbmDevice)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create GBM device" << std::endl;
        close(_drmFD); _drmFD = -1; return false;
    }
    
    _gbmSurface = gbm_surface_create(_gbmDevice, _width, _height, GBM_FORMAT_XRGB8888,
                                     GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!_gbmSurface)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create GBM surface" << std::endl;
        gbm_device_destroy(_gbmDevice); _gbmDevice = nullptr;
        close(_drmFD); _drmFD = -1; return false;
    }
    return true;
}

bool VaapiDecoder::setupVAAPI()
{
    _vaDisplay = vaGetDisplayDRM(_drmFD);
    if (!_vaDisplay)
    {
        OSG_WARN << "[VaapiDecoder] Failed to get VA display" << std::endl;
        return false;
    }
    
    int majorVer = 0, minorVer = 0;
    VAStatus status = vaInitialize(_vaDisplay, &majorVer, &minorVer);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to initialize VA-API: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    
    VAProfile profile = VAProfileNone;
    switch (_videoCodec)
    {
    case 0: // H.264
        profile = VAProfileH264High; break;
    case 1: // HEVC
        profile = VAProfileHEVCMain; break;
    case 2: // VP9
        profile = VAProfileVP9Profile0; break;
    default:
        OSG_WARN << "[VaapiDecoder] Unsupported codec: " << _videoCodec << std::endl;
        return false;
    }
    
    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;  // FIXME
    status = vaCreateConfig(_vaDisplay, profile, VAEntrypointVLD, &attrib, 1, &_vaConfig);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA config: " << vaErrorStr(status) << std::endl;
        vaTerminate(_vaDisplay); _vaDisplay = nullptr; return false;
    }
    return true;
}

bool VaapiDecoder::createSurface()
{
    VASurfaceAttrib attribs[1];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME; 

    VAStatus status = vaCreateSurfaces(_vaDisplay, VA_RT_FORMAT_YUV420,
                                       _width, _height, &_vaSurface, 1, attribs, 1);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA surface: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    
    status = vaCreateContext(_vaDisplay, _vaConfig, _width, _height,
                             VA_PROGRESSIVE, &_vaSurface, 1, &_vaContext);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA context: "
                 << vaErrorStr(status) << std::endl;
        vaDestroySurfaces(_vaDisplay, &_vaSurface, 1);
        _vaSurface = VA_INVALID_SURFACE; return false;
    }
    return true;
}

bool VaapiDecoder::decode(const uint8_t* data, int size, long long pts)
{
    VABufferID buffers[1];
    if (!_initialized || !data || size <= 0) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    
    VAStatus status = vaCreateBuffer(_vaDisplay, _vaContext, VASliceDataBufferType,
                                     size, 1, (void*)data, &buffers[0]);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA buffer: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    
    status = vaBeginPicture(_vaDisplay, _vaContext, _vaSurface);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to begin picture: " << vaErrorStr(status) << std::endl;
        vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }
    
    status = vaRenderPicture(_vaDisplay, _vaContext, buffers, 1);
    if (status != VA_STATUS_SUCCESS) {
        OSG_WARN << "[VaapiDecoder] Failed to render picture: " << vaErrorStr(status) << std::endl;
        vaEndPicture(_vaDisplay, _vaContext); vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }
    
    status = vaEndPicture(_vaDisplay, _vaContext);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to end picture: " << vaErrorStr(status) << std::endl;
        vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }
    
    vaDestroyBuffer(_vaDisplay, buffers[0]);
    _currentPTS = pts; _frameReady = true; return true;
}

bool VaapiDecoder::syncSurface()
{
    if (!_initialized || _vaSurface == VA_INVALID_SURFACE) return false;
    VAStatus status = vaSyncSurface(_vaDisplay, _vaSurface);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaSyncSurface failed: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    return true;
}

bool VaapiDecoder::exportDecodedFrame(std::vector<ExportedBufferData>& bufferData)
{
    if (!_initialized || !_frameReady || _vaSurface == VA_INVALID_SURFACE) return -1;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!syncSurface()) return -1;

    VADRMPRIMESurfaceDescriptor desc = {};
    VAStatus status = vaExportSurfaceHandle(
        _vaDisplay, _vaSurface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (status != VA_STATUS_SUCCESS)
    {
        status = vaExportSurfaceHandle(
            _vaDisplay, _vaSurface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
            VA_EXPORT_SURFACE_READ_ONLY, &desc);
    }
    
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaExportSurfaceHandle failed: "
                 << vaErrorStr(status) << std::endl; return -1;
    }
    else if (desc.num_objects < 1 || desc.num_layers < 1)
    {
        OSG_WARN << "[VaapiDecoder] Invalid export descriptor" << std::endl;
        return -1;
    }
    
    for (uint32_t i = 0; i < desc.num_layers; i++)
    {
        for (uint32_t j = 0; j < desc.layers[i].num_planes; j++)
        {
            uint32_t obj_idx = desc.layers[i].object_index[j];
            if (obj_idx < desc.num_objects)
            {
                ExportedBufferData exported = {};
                exported.fd = dup(desc.objects[obj_idx].fd);
                exported.drmPrimePitch = desc.layers[i].pitch[j];
                exported.drmPrimeOffset = desc.layers[i].offset[j];
                exported.drmPrimeModifier = desc.objects[obj_idx].drm_format_modifier;
                bufferData.push_back(exported);
            }
        }
    }
    
    for (uint32_t i = 0; i < desc.num_objects; i++)
    { if (desc.objects[i].fd >= 0) close(desc.objects[i].fd); }
    return !bufferData.empty();
}

bool VaapiDecoder::getDecodedFrame(uint8_t** frameData, int* pitch, long long* pts)
{
    if (!_frameReady || !_vaSurface) return false;
    VAImage image = {}; void* imageData = nullptr;

    VAStatus status = vaDeriveImage(_vaDisplay, _vaSurface, &image);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to derive image: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    
    status = vaMapBuffer(_vaDisplay, image.buf, &imageData);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to map image buffer: " << vaErrorStr(status) << std::endl;
        vaDestroyImage(_vaDisplay, image.image_id); return false;
    }
    
    int frameSize = _width * _height * 4;
    uint8_t* rgbaData = new uint8_t[frameSize];
    
    // TODO: yuv to rgba?
#if false
    uint8_t* yPlane = (uint8_t*)imageData + image.offsets[0];
    uint8_t* uPlane = (uint8_t*)imageData + image.offsets[1];
    uint8_t* vPlane = (uint8_t*)imageData + image.offsets[2];
    for (int y = 0; y < _height; y++) {
        for (int x = 0; x < _width; x++) {
            int yIdx = y * image.pitches[0] + x;
            int uvIdx = (y / 2) * image.pitches[1] + (x / 2);
            
            int Y = yPlane[yIdx];
            int U = uPlane[uvIdx] - 128;
            int V = vPlane[uvIdx] - 128;
            
            int R = Y + 1.402f * V;
            int G = Y - 0.344f * U - 0.714f * V;
            int B = Y + 1.772f * U;
            
            int rgbaIdx = y * _width * 4 + x * 4;
            rgbaData[rgbaIdx + 0] = (uint8_t)std::clamp(R, 0, 255);
            rgbaData[rgbaIdx + 1] = (uint8_t)std::clamp(G, 0, 255);
            rgbaData[rgbaIdx + 2] = (uint8_t)std::clamp(B, 0, 255);
            rgbaData[rgbaIdx + 3] = 255;
        }
    }
#endif
    vaUnmapBuffer(_vaDisplay, image.buf);
    vaDestroyImage(_vaDisplay, image.image_id);
    *frameData = rgbaData; *pitch = _width * 4;
    *pts = _currentPTS; _frameReady = false; return true;
}

void VaapiDecoder::releaseFrame()
{
    //
}

void VaapiDecoder::cleanupVAAPI()
{
    if (_vaContext != VA_INVALID_ID)
    { vaDestroyContext(_vaDisplay, _vaContext); _vaContext = VA_INVALID_ID; }
    
    if (_vaSurface != VA_INVALID_SURFACE)
    { vaDestroySurfaces(_vaDisplay, &_vaSurface, 1); _vaSurface = VA_INVALID_SURFACE; }
    
    if (_vaConfig != VA_INVALID_ID)
    { vaDestroyConfig(_vaDisplay, _vaConfig); _vaConfig = VA_INVALID_ID; }
    
    if (_vaDisplay)
    { vaTerminate(_vaDisplay); _vaDisplay = nullptr; }
}

void VaapiDecoder::cleanupGBM()
{
    if (_gbmBuffer) { gbm_bo_destroy(_gbmBuffer); _gbmBuffer = nullptr; }
    if (_gbmSurface) { gbm_surface_destroy(_gbmSurface); _gbmSurface = nullptr; }
    if (_gbmDevice) { gbm_device_destroy(_gbmDevice); _gbmDevice = nullptr; }
    if (_drmFD >= 0) { close(_drmFD); _drmFD = -1; }
}
