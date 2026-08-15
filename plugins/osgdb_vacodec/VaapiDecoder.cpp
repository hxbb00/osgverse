#include <osg/Notify>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include "VaapiDecoder.h"
using namespace osgVerse;

VaapiDecoder::VaapiDecoder(int videoCodec, int width, int height)
:   _vaDisplay(nullptr), _vaConfig(VA_INVALID_ID), _vppConfig(VA_INVALID_ID), _vaContext(VA_INVALID_ID),
    _vppContext(_vppContext), _vaSurface(VA_INVALID_SURFACE), _vaSurfaceRGB(VA_INVALID_SURFACE),
    _currentPTS(0), _framebufferID(0), _videoCodec(videoCodec), _drmFD(-1),
    _width(width), _height(height), _bitDepth(8), _initialized(false), _frameReady(false)
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
    case 0:  /**<  MPEG1             */ profile = VAProfileMPEG2Simple; break;
    case 1:  /**<  MPEG2             */ profile = VAProfileMPEG2Main; break;
    case 2:  /**<  MPEG4             */ profile = VAProfileMPEG4Main; break;
    case 3:  /**<  VC1               */ profile = VAProfileVC1Main; break;
    case 4:  /**<  H264              */ profile = VAProfileH264Main; break;
    case 5:  /**<  JPEG              */ profile = VAProfileJPEGBaseline; break;
    case 8:  /**<  HEVC              */ profile = VAProfileHEVCMain; break;
    case 9:  /**<  VP8               */ profile = VAProfileVP8Version0_3; break;
    case 10: /**<  VP9               */ profile = VAProfileVP9Profile0; break;
    case 11: /**<  AV1               */ profile = VAProfileAV1Profile0; break;
    default:
        OSG_WARN << "[VaapiDecoder] Unsupported codec: " << _videoCodec << std::endl;
        return false;
    }
    
    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    status = vaCreateConfig(_vaDisplay, profile, VAEntrypointVLD, &attrib, 1, &_vaConfig);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA config: " << vaErrorStr(status) << std::endl;
        vaTerminate(_vaDisplay); _vaDisplay = nullptr; return false;
    }
    
    VAConfigAttrib attrib2 = {};
    attrib2.type = VAConfigAttribRTFormat;
    attrib2.value = VA_RT_FORMAT_RGB32;
    status = vaCreateConfig(_vaDisplay, VAProfileNone, VAEntrypointVideoProc,
                            &attrib2, 1, &_vppConfig);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VPP config: " << vaErrorStr(status) << std::endl;
        return false;
    }
    return true;
}

bool VaapiDecoder::createSurface()
{
    // Original surface for YUV420 input
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
        OSG_WARN << "[VaapiDecoder] Failed to create VA context: " << vaErrorStr(status) << std::endl;
        vaDestroySurfaces(_vaDisplay, &_vaSurface, 1);
        _vaSurface = VA_INVALID_SURFACE; return false;
    }

    // Surface for converted RGB output
    VASurfaceAttrib rgbAttribs[1];
    rgbAttribs[0].type = VASurfaceAttribMemoryType;
    rgbAttribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    rgbAttribs[0].value.type = VAGenericValueTypeInteger;
    rgbAttribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    
    status = vaCreateSurfaces(_vaDisplay, VA_RT_FORMAT_RGB32,
                              _width, _height, &_vaSurfaceRGB, 1, rgbAttribs, 1);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create RGB surface: " << vaErrorStr(status) << std::endl;
        _vaSurfaceRGB = VA_INVALID_SURFACE; vaDestroyConfig(_vaDisplay, _vppConfig);
        _vppConfig = VA_INVALID_ID; return false;
    }

    status = vaCreateContext(_vaDisplay, _vppConfig, _width, _height,
                             VA_PROGRESSIVE, &_vaSurfaceRGB, 1, &_vppContext);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VPP context: " << vaErrorStr(status) << std::endl;
        vaDestroySurfaces(_vaDisplay, &_vaSurfaceRGB, 1);
        _vaSurfaceRGB = VA_INVALID_SURFACE; vaDestroyConfig(_vaDisplay, _vppConfig);
        _vppConfig = VA_INVALID_ID; return false;
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
    
    syncSurface(true); vaDestroyBuffer(_vaDisplay, buffers[0]);
    _currentPTS = pts; _frameReady = true; return true;
}

bool VaapiDecoder::convert()
{
    VABufferID buffers[1];
    if (!_initialized || !_frameReady || _vaSurface == VA_INVALID_SURFACE) return false;
    std::lock_guard<std::mutex> lock(_mutex);

    VAProcPipelineParameterBuffer pipelineParam;
    memset(&pipelineParam, 0, sizeof(pipelineParam));
    pipelineParam.surface = _vaSurface;
    pipelineParam.surface_region = nullptr;
    pipelineParam.output_region = nullptr;
    pipelineParam.output_background_color = 0;
    pipelineParam.filter_flags = 0;
    
    VAStatus status = vaCreateBuffer(_vaDisplay, _vppContext, VAProcPipelineParameterBufferType,
                                     sizeof(pipelineParam), 1, &pipelineParam, &buffers[0]);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VPP buffer: "
                 << vaErrorStr(status) << std::endl; return false;
    }

    status = vaBeginPicture(_vaDisplay, _vppContext, _vaSurfaceRGB);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] VPP BeginPicture failed: " << vaErrorStr(status) << std::endl;
        vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }
    
    status = vaRenderPicture(_vaDisplay, _vppContext, &buffers[0], 1);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] VPP RenderPicture failed: " << vaErrorStr(status) << std::endl;
        vaEndPicture(_vaDisplay, _vppContext); vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }

    status = vaEndPicture(_vaDisplay, _vppContext);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] VPP EndPicture failed: " << vaErrorStr(status) << std::endl;
        vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }

    syncSurface(false); vaDestroyBuffer(_vaDisplay, buffers[0]);
    return true;
}

bool VaapiDecoder::syncSurface(bool origin)
{
    VASurfaceID surface = (origin ? _vaSurface : _vaSurfaceRGB);
    if (!_initialized || surface == VA_INVALID_SURFACE) return false;

    VAStatus status = vaSyncSurface(_vaDisplay, surface);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaSyncSurface failed: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    return true;
}

bool VaapiDecoder::exportDecodedFrame(int& fourcc, std::vector<ExportedBufferData>& bufferData)
{
    if (!_initialized || _vaSurfaceRGB == VA_INVALID_SURFACE) return false;
    std::lock_guard<std::mutex> lock(_mutex);

    VADRMPRIMESurfaceDescriptor desc = {};
    VAStatus status = vaExportSurfaceHandle(
        _vaDisplay, _vaSurfaceRGB, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (status != VA_STATUS_SUCCESS)
    {
        status = vaExportSurfaceHandle(
            _vaDisplay, _vaSurfaceRGB, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
            VA_EXPORT_SURFACE_READ_ONLY, &desc);
    }
    
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaExportSurfaceHandle failed: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    else if (desc.num_objects < 1 || desc.num_layers < 1)
    {
        OSG_WARN << "[VaapiDecoder] Invalid export descriptor" << std::endl;
        return false;
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
    fourcc = desc.fourcc; return !bufferData.empty();
}

void VaapiDecoder::releaseFrame()
{
    _frameReady = false;
}

void VaapiDecoder::cleanupVAAPI()
{
    if (_vppContext != VA_INVALID_ID)
    { vaDestroyContext(_vaDisplay, _vppContext); _vppContext = VA_INVALID_ID; }
    if (_vaContext != VA_INVALID_ID)
    { vaDestroyContext(_vaDisplay, _vaContext); _vaContext = VA_INVALID_ID; }
    
    if (_vaSurfaceRGB != VA_INVALID_SURFACE)
    { vaDestroySurfaces(_vaDisplay, &_vaSurfaceRGB, 1); _vaSurfaceRGB = VA_INVALID_SURFACE; }
    if (_vaSurface != VA_INVALID_SURFACE)
    { vaDestroySurfaces(_vaDisplay, &_vaSurface, 1); _vaSurface = VA_INVALID_SURFACE; }
    
    if (_vppConfig != VA_INVALID_ID)
    { vaDestroyConfig(_vaDisplay, _vppConfig); _vppConfig = VA_INVALID_ID; }
    if (_vaConfig != VA_INVALID_ID)
    { vaDestroyConfig(_vaDisplay, _vaConfig); _vaConfig = VA_INVALID_ID; }
    
    if (_vaDisplay)
    { vaTerminate(_vaDisplay); _vaDisplay = nullptr; }
}

void VaapiDecoder::cleanupGBM()
{
    if (_drmFD >= 0) { close(_drmFD); _drmFD = -1; }
}
