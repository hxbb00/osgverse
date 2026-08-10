#include <iostream>
#include <osg/Version>
#include <osg/io_utils>
#include <osg/GLExtensions>
#include <osg/FrameBufferObject>
#include "ExternalTexture2D.h"

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
#   define __GL_H__  // don't include GL/gl.h
#endif

#ifdef VERSE_ENABLE_MTT
#   include <musa.h>
#   include <musaGL.h>
#else
#   include <cuda.h>
#   include <cudaGL.h>
#endif
using namespace osgVerse;

inline bool check(int e, int iLine, const char* szFile)
{
    if (e < 0)
    {
        OSG_WARN << "General error " << e << " at line " << iLine << " in file " << szFile;
        return false;
    }
    return true;
}
#define ck(call) check(call, __LINE__, __FILE__)

GpuResourceWriterBase::GpuResourceWriterBase(CUcontext cu)
:   osg::Camera::DrawCallback(), _muxerParent(NULL)
{ _cuContext = (CUcontext)cu; }

bool GpuResourceWriterBase::openResource(GpuResourceDemuxerMuxerContainer* c)
{
    _muxerParent = NULL; if (!c) return false;
    if (c->getMuxer()) return openResource(c->getMuxer());
    else { _muxerParent = c; return true; }
}

GpuResourceReaderBase::GpuResourceReaderBase(CUcontext cu)
:   osg::Texture2D::SubloadCallback(), _textureID(0), _state(INVALID), _width(0), _height(0), _vendorStatus(false)
{ _handle = new CudaResourceHandle(cu); _resourceType = RES_CUDA; }

GpuResourceReaderBase::GpuResourceReaderBase(void* eglDisplay, void* eglContext)
:   osg::Texture2D::SubloadCallback(), _textureID(0), _state(INVALID), _width(0), _height(0), _vendorStatus(false)
{ _handle = new EglResourceHandle(eglDisplay, eglContext); _resourceType = RES_EGL; }

bool GpuResourceReaderBase::openResource(GpuResourceDemuxerMuxerContainer* c)
{ return (c && c->getDemuxer()) ? openResource(c->getDemuxer()) : false; }

void GpuResourceReaderBase::releaseGpu()
{
    if (_resourceType == RES_CUDA)
    {
        CudaResourceHandle* H = static_cast<CudaResourceHandle*>(_handle.get());
        if (H->cuResource != NULL)
        {
#ifdef VERSE_ENABLE_MTT
            ck(muGraphicsUnregisterResource(H->cuResource));
#else
            ck(cuGraphicsUnregisterResource(H->cuResource));
#endif
        }
#ifdef VERSE_ENABLE_MTT
        _mutex.lock(); ck(muMemFree(H->deviceFrame)); _mutex.unlock();
#else
        _mutex.lock(); ck(cuMemFree(H->deviceFrame)); _mutex.unlock();
#endif
        H->cuResource = NULL;
    }
    else if (_resourceType == RES_EGL)
    {
        // TODO: release EGL image
    }
    _demuxer = NULL;
}

void GpuResourceReaderBase::releaseGLObjects(osg::State* state) const
{
#if OSG_VERSION_GREATER_THAN(3, 3, 2)
    osg::GLExtensions* ext = state->get<osg::GLExtensions>();
#else
    osg::GLBufferObject::Extensions* ext = osg::GLBufferObject::getExtensions(state->getContextID(), true);
#endif
    if (_resourceType == RES_CUDA)
    {
        CudaResourceHandle* H = static_cast<CudaResourceHandle*>(_handle.get());
        if (H && H->pbo != 0) { ext->glDeleteBuffers(1, &(H->pbo)); H->pbo = 0; }
    }
    else if (_resourceType == RES_EGL)
    {
        // TODO
    }
}

#if OSG_VERSION_GREATER_THAN(3, 4, 0)
osg::ref_ptr<osg::Texture::TextureObject> GpuResourceReaderBase::generateTextureObject(
            const osg::Texture2D& texture, osg::State& state) const
#else
osg::Texture::TextureObject* GpuResourceReaderBase::generateTextureObject(
            const osg::Texture2D& texture, osg::State& state) const
#endif
{
    osg::ref_ptr<osg::Texture::TextureObject> obj =
        osg::Texture::generateTextureObject(&texture, state.getContextID(), GL_TEXTURE_2D);
    _textureID = obj->id(); return obj.get();
}

void GpuResourceReaderBase::setDefaultTestImage(osg::Image* image)
{
    if (!image) return;
    if (image->getPixelFormat() == GL_RGBA || image->getPixelFormat() == GL_BGRA)
    {
        if (!image->valid()) return;
        if (image->getDataType() == GL_UNSIGNED_BYTE)
        {
            _testImage = image;
            _width = image->s();  _height = image->t();
        }
    }
}

void GpuResourceReaderBase::load(const osg::Texture2D& texture, osg::State& state) const
{
    char* vendor = (char*)glGetString(GL_VENDOR);
    if (std::string(vendor).find("NVIDIA") != std::string::npos) _vendorStatus = true;
    //else { _vendorStatus = false; return; }

#if OSG_VERSION_GREATER_THAN(3, 3, 2)
    osg::GLExtensions* ext = state.get<osg::GLExtensions>();
#else
    osg::GLBufferObject::Extensions* ext = osg::GLBufferObject::getExtensions(state.getContextID(), true);
#endif
    if (_width == 0 || _height == 0 || !ext) return;

    if (_resourceType == RES_CUDA)
    {
        CudaResourceHandle* H = static_cast<CudaResourceHandle*>(_handle.get());
        if (H->pbo != 0) ext->glDeleteBuffers(1, &(H->pbo));

        unsigned int imageSize = _width * _height * 4;
        ext->glGenBuffers(1, &(H->pbo));
        ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, H->pbo);
        ext->glBufferData(GL_PIXEL_UNPACK_BUFFER_ARB, imageSize, NULL, GL_STREAM_DRAW_ARB);
        ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);

#ifdef VERSE_ENABLE_MTT
        ck(muCtxSetCurrent(H->cuContext));
        ck(muMemAlloc(&(H->deviceFrame), imageSize));
        if (_testImage.valid() && _testImage->getTotalSizeInBytes() == imageSize)
            ck(muMemcpyHtoD(H->deviceFrame, _testImage->data(), imageSize));
        else
            ck(muMemsetD8(H->deviceFrame, 0, imageSize));
#else
        ck(cuCtxSetCurrent(H->cuContext));
        ck(cuMemAlloc(&(H->deviceFrame), imageSize));
        if (_testImage.valid() && _testImage->getTotalSizeInBytes() == imageSize)
            ck(cuMemcpyHtoD(H->deviceFrame, _testImage->data(), imageSize));
        else
            ck(cuMemsetD8(H->deviceFrame, 0, imageSize));
#endif
    }
    else if (_resourceType == RES_EGL)
    {
        // TODO: create image, load function address
    }

    glBindTexture(GL_TEXTURE_2D, _textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GpuResourceReaderBase::subload(const osg::Texture2D& texture, osg::State& state) const
{
#if OSG_VERSION_GREATER_THAN(3, 3, 2)
    osg::GLExtensions* ext = state.get<osg::GLExtensions>();
#else
    osg::GLBufferObject::Extensions* ext = osg::GLBufferObject::getExtensions(state.getContextID(), true);
#endif
    if (_resourceType == RES_CUDA)
    {
        CudaResourceHandle* H = static_cast<CudaResourceHandle*>(_handle.get());
        if (_width == 0 || _height == 0) return;
        if (H->pbo == 0) { load(texture, state); if (H->pbo == 0) return; }
        _mutex.lock();

        CUdeviceptr devBackBuffer; size_t size = 0;
#ifdef VERSE_ENABLE_MTT
        if (!H->cuResource) ck(muGraphicsGLRegisterBuffer(&(H->cuResource), H->pbo, MU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
        ck(muGraphicsMapResources(1, &(H->cuResource), 0));
        ck(muGraphicsResourceGetMappedPointer(&devBackBuffer, &size, H->cuResource));
        ck(muMemcpyAsync(devBackBuffer, H->deviceFrame, size, 0));
        ck(muGraphicsUnmapResources(1, &(H->cuResource), 0));
#else
        if (!H->cuResource) ck(cuGraphicsGLRegisterBuffer(&(H->cuResource), H->pbo, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
        ck(cuGraphicsMapResources(1, &(H->cuResource), 0));
        ck(cuGraphicsResourceGetMappedPointer(&devBackBuffer, &size, H->cuResource));
        ck(cuMemcpyAsync(devBackBuffer, H->deviceFrame, size, 0));
        ck(cuGraphicsUnmapResources(1, &(H->cuResource), 0));
#endif
        _mutex.unlock();

        if (ext) ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, H->pbo);
        glBindTexture(GL_TEXTURE_2D, _textureID);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _width, _height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        if (ext) ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    }
    else if (_resourceType == RES_EGL)
    {
        // TODO: bind and show image with glEGLImageTargetTexStorageEXT / glEGLImageTargetTexture2DOES
    }
}

bool GpuResourceReaderBase::getDeviceFrameBuffer(CUdeviceptr* devFrameOut, int* pitchOut)
{
    // FIXME: consider use a queue because reader may return multiple data in one frame
    if (_resourceType == RES_CUDA)
    {
        CudaResourceHandle* H = static_cast<CudaResourceHandle*>(_handle.get());
        if (!H->deviceFrame) return false;
        *devFrameOut = (CUdeviceptr)H->deviceFrame;
    }
    else if (_resourceType == RES_EGL)
    {
        // TODO: update EGL image? or in a different method?
    }
    *pitchOut = _width * 4; return true;
}

namespace
{
    class ResourceUpdateCallback : public osg::StateAttribute::Callback
    {
    public:
        ResourceUpdateCallback(GpuResourceReaderBase* cb)
            : _manager(cb), _lastTick(0) {}

        virtual void operator()(osg::StateAttribute* sa, osg::NodeVisitor* nv)
        {
            osg::Timer_t now = osg::Timer::instance()->tick();
            if (_manager.valid())
            {
                double fps = _manager->getDemuxer() ? _manager->getDemuxer()->getFrameRate() : 25.0;
                double step = fps > 1.0 ? (1000.0 / fps) : 50.0;  // target msecs between two frames
                double sec = osg::Timer::instance()->delta_m(_lastTick, now);
                if (step <= sec) { (*_manager)(sa, nv); _lastTick = now; }
            }
        }

    protected:
        osg::observer_ptr<GpuResourceReaderBase> _manager;
        osg::Timer_t _lastTick;
    };
}

ExternalTexture2D::ExternalTexture2D(void* cu) : osg::Texture2D(), _cuContext(cu)
{}

ExternalTexture2D::ExternalTexture2D(const ExternalTexture2D& copy, const osg::CopyOp& op)
:   osg::Texture2D(copy, op), _cuContext(copy._cuContext)
{}

ExternalTexture2D::~ExternalTexture2D()
{
    GpuResourceReaderBase* callback = static_cast<GpuResourceReaderBase*>(getSubloadCallback());
    if (callback) callback->releaseGpu();
}

void ExternalTexture2D::setResourceReader(GpuResourceReaderBase* reader)
{
    ResourceUpdateCallback* cb = NULL;
    if (reader) cb = new ResourceUpdateCallback(reader);
    setSubloadCallback(reader); setUpdateCallback(cb);
}

const GpuResourceReaderBase* ExternalTexture2D::getResourceReader() const
{ return dynamic_cast<const GpuResourceReaderBase*>(getSubloadCallback()); }

void ExternalTexture2D::releaseGLObjects(osg::State* state) const
{
    const GpuResourceReaderBase* callback = static_cast<const GpuResourceReaderBase*>(getSubloadCallback());
    if (callback) callback->releaseGLObjects(state);
    osg::Texture2D::releaseGLObjects(state);
}

void ExternalTexture2D::releaseGpuData()
{
    GpuResourceReaderBase* callback = static_cast<GpuResourceReaderBase*>(getSubloadCallback());
    if (callback) callback->releaseGpu();
}
