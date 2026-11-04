//
// osgVerse::GraphicsWindowWGPU implementation. See the header for the overview.
//

#include "GraphicsWindowWGPU.h"
#include <osg/Notify>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <emscripten/html5.h>
#include <cstring>
#include <iostream>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#   define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif

namespace osgVerse
{

namespace
{
    // The page provides the canvas; its CSS selector is what the WebGPU surface
    // source takes. The demo passes the same string to eglCreateWindowSurface.
    const char* kDefaultCanvasSelector = "#canvas";
}

GraphicsWindowWGPU::GraphicsWindowWGPU(osg::GraphicsContext::Traits* traits)
    : osgViewer::GraphicsWindowEmbedded(traits),
      _eglDisplay(NULL), _eglSurface(NULL), _eglContext(NULL), _eglInitialized(false)
{
}

GraphicsWindowWGPU::~GraphicsWindowWGPU()
{
    closeImplementation();
}

bool GraphicsWindowWGPU::initEgl(int& outWidth, int& outHeight)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (eglGetPlatformDisplayEXT == NULL)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglGetPlatformDisplayEXT() is not available"
                 << std::endl;
        return false;
    }

    // The only back-end gles-on-webgpu installs behind this platform type.
    const EGLint displayAttribs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_WEBGPU_ANGLE,
        EGL_NONE,
    };
    EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, displayAttribs);
    if (display == EGL_NO_DISPLAY)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglGetPlatformDisplayEXT(WebGPU) failed" << std::endl;
        return false;
    }

    EGLint major = 0, minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglInitialize() failed: " << std::hex << eglGetError()
                 << std::dec << std::endl;
        return false;
    }
    OSG_NOTICE << "[GraphicsWindowWGPU] EGL " << major << "." << minor << " - "
               << eglQueryString(display, EGL_VENDOR) << std::endl;

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglBindAPI() failed" << std::endl;
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        int(_traits.valid() ? _traits->red : 8),
        EGL_GREEN_SIZE,      int(_traits.valid() ? _traits->green : 8),
        EGL_BLUE_SIZE,       int(_traits.valid() ? _traits->blue : 8),
        EGL_ALPHA_SIZE,      int(_traits.valid() ? _traits->alpha : 8),
        EGL_DEPTH_SIZE,      int(_traits.valid() ? _traits->depth : 24),
        EGL_STENCIL_SIZE,    int(_traits.valid() ? _traits->stencil : 8),
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint numConfigs = 0;
    if (eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
        numConfigs < 1)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglChooseConfig() found no EGL_WINDOW_BIT config"
                 << std::endl;
        return false;
    }

    // On Emscripten EGLNativeWindowType is an int, so the selector string travels
    // as the pointer it is and the back-end turns it into a
    // WGPUEmscriptenSurfaceSourceCanvasHTMLSelector.
    const EGLNativeWindowType nativeWindow =
        static_cast<EGLNativeWindowType>(reinterpret_cast<uintptr_t>(kDefaultCanvasSelector));
    EGLSurface surface = eglCreateWindowSurface(display, config, nativeWindow, NULL);
    if (surface == EGL_NO_SURFACE)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglCreateWindowSurface() failed: " << std::hex
                 << eglGetError() << std::dec << std::endl;
        return false;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglCreateContext() failed: " << std::hex
                 << eglGetError() << std::dec << std::endl;
        eglDestroySurface(display, surface);
        return false;
    }

    // The canvas size is authoritative: the surface was configured from it.
    int canvasWidth = 0, canvasHeight = 0;
    if (emscripten_get_canvas_element_size(kDefaultCanvasSelector, &canvasWidth, &canvasHeight) ==
            EMSCRIPTEN_RESULT_SUCCESS &&
        canvasWidth > 0 && canvasHeight > 0)
    {
        outWidth  = canvasWidth;
        outHeight = canvasHeight;
    }

    _eglDisplay = (void*)display;
    _eglSurface = (void*)surface;
    _eglContext = (void*)context;
    return true;
}

bool GraphicsWindowWGPU::realizeImplementation()
{
    if (_eglInitialized) return true;

    int width  = _traits.valid() ? _traits->width : 800;
    int height = _traits.valid() ? _traits->height : 600;
    if (!initEgl(width, height)) return false;
    _eglInitialized = true;

    if (_traits.valid())
    {
        if (width > 0) _traits->width = width;
        if (height > 0) _traits->height = height;
    }

    if (!makeCurrentImplementation()) return false;
    return osgViewer::GraphicsWindowEmbedded::realizeImplementation();
}

bool GraphicsWindowWGPU::makeCurrentImplementation()
{
    if (_eglDisplay == NULL || _eglContext == NULL) return false;
    return eglMakeCurrent((EGLDisplay)_eglDisplay, (EGLSurface)_eglSurface, (EGLSurface)_eglSurface,
                          (EGLContext)_eglContext) == EGL_TRUE;
}

bool GraphicsWindowWGPU::releaseContextImplementation()
{
    // gles-on-webgpu has a single context and nothing else in the page uses EGL;
    // keeping it current is what GraphicsWindowEmbedded assumes anyway.
    return true;
}

void GraphicsWindowWGPU::swapBuffersImplementation()
{
    if (_eglDisplay == NULL || _eglSurface == NULL) return;

    // This is where a frame actually reaches the GPU: eglSwapBuffers ends the
    // render pass, finishes the command encoder and submits it. The browser then
    // presents the canvas when the animation-frame callback returns.
    if (eglSwapBuffers((EGLDisplay)_eglDisplay, (EGLSurface)_eglSurface) != EGL_TRUE)
    {
        OSG_WARN << "[GraphicsWindowWGPU] eglSwapBuffers() failed: " << std::hex << eglGetError()
                 << std::dec << std::endl;
    }
}

void GraphicsWindowWGPU::closeImplementation()
{
    if (_eglDisplay == NULL) return;

    EGLDisplay display = (EGLDisplay)_eglDisplay;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (_eglContext) eglDestroyContext(display, (EGLContext)_eglContext);
    if (_eglSurface) eglDestroySurface(display, (EGLSurface)_eglSurface);
    eglTerminate(display);

    _eglDisplay = NULL;
    _eglSurface = NULL;
    _eglContext = NULL;
    _eglInitialized = false;
}

// ---------------------------------------------------------------------------
// Windowing system registration, so demos can simply ask for
// traits->windowingSystemPreference = "WGPU".
// ---------------------------------------------------------------------------
namespace
{
    class WGPUWindowingSystemInterface : public osg::GraphicsContext::WindowingSystemInterface
    {
    public:
        typedef osg::GraphicsContext::ScreenIdentifier ScreenIdentifier;
        typedef osg::GraphicsContext::ScreenSettings ScreenSettings;
        typedef osg::GraphicsContext::ScreenSettingsList ScreenSettingsList;

        WGPUWindowingSystemInterface() { setName("WGPU"); }

        virtual unsigned int getNumScreens(const ScreenIdentifier& = ScreenIdentifier())
        {
            return 1;
        }

        virtual void getScreenSettings(const ScreenIdentifier&, ScreenSettings& settings)
        {
            int width = 800, height = 600;
            emscripten_get_canvas_element_size(kDefaultCanvasSelector, &width, &height);
            settings.width = width; settings.height = height; settings.refreshRate = 60.0;
        }

        virtual void enumerateScreenSettings(const ScreenIdentifier&,
                                             ScreenSettingsList& settingsList)
        {
            settingsList.clear();
            ScreenSettings settings; getScreenSettings(ScreenIdentifier(), settings);
            settingsList.push_back(settings);
        }

        virtual osg::GraphicsContext* createGraphicsContext(osg::GraphicsContext::Traits* traits)
        {
            if (!traits) return NULL;
            return new GraphicsWindowWGPU(traits);
        }
    };
}

void registerWGPUWindowingSystem()
{
    static bool registered = false;
    if (registered) return;
    registered = true;
    osg::GraphicsContext::getWindowingSystemInterfaces()->addWindowingSystemInterface(
        new WGPUWindowingSystemInterface);
}

}  // namespace osgVerse

// Referenced by USE_GRAPICSWINDOW_IMPLEMENTATION(WGPU).
extern "C" void graphicswindow_WGPU(void)
{
    osgVerse::registerWGPUWindowingSystem();
}
