//
// osgVerse::GraphicsWindowWGPU - an OpenGL ES 3 context on the page's canvas,
// with every gl*() call implemented by gles-on-webgpu on top of WebGPU.
//
// It is the WebGPU counterpart of GraphicsWindowSDL: the same "create an EGL
// display, pick a config, make a window surface and a context" sequence, except
// that
//   * eglGetPlatformDisplayEXT() is asked for EGL_PLATFORM_ANGLE_TYPE_WEBGPU_ANGLE,
//   * the native window is the CSS selector of the page's canvas (on Emscripten
//     EGLNativeWindowType is an int, so the string travels as a pointer), and
//   * the frame is submitted by eglSwapBuffers(), because the WebGPU back-end
//     queues everything into a command encoder that only reaches the GPU there.
//
// Register it once with USE_GRAPICSWINDOW_IMPLEMENTATION(WGPU) and select it
// with traits->windowingSystemPreference = "WGPU".
//

#ifndef OSGVERSE_GRAPHICSWINDOWWGPU_H
#define OSGVERSE_GRAPHICSWINDOWWGPU_H

#include <osgViewer/GraphicsWindow>

namespace osgVerse
{
    class GraphicsWindowWGPU : public osgViewer::GraphicsWindowEmbedded
    {
    public:
        GraphicsWindowWGPU(osg::GraphicsContext::Traits* traits);

        /** EGL is created here rather than in the constructor so the caller can
            still decide not to use the window. */
        virtual bool realizeImplementation();
        virtual bool makeCurrentImplementation();
        virtual bool releaseContextImplementation();
        virtual void swapBuffersImplementation();
        virtual void closeImplementation();

        bool isEglReady() const { return _eglDisplay != NULL && _eglContext != NULL; }

    protected:
        virtual ~GraphicsWindowWGPU();

        bool initEgl(int& outWidth, int& outHeight);

        void* _eglDisplay;
        void* _eglSurface;
        void* _eglContext;
        bool _eglInitialized;
    };

    /** Register the "WGPU" windowing system with osg::GraphicsContext.
        USE_GRAPICSWINDOW_IMPLEMENTATION(WGPU) does this automatically. */
    extern void registerWGPUWindowingSystem();
}

#endif
