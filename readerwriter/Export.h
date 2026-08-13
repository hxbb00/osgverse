#ifndef MANA_READERWRITER_EXPORT_HPP
#define MANA_READERWRITER_EXPORT_HPP

#include <osg/Referenced>
#if defined(VERSE_STATIC_BUILD)
#  define OSGVERSE_RW_EXPORT
#elif defined(VERSE_WINDOWS)
#  if defined(VERSE_RW_LIBRARY)
#    define OSGVERSE_RW_EXPORT __declspec(dllexport)
#  else
#    define OSGVERSE_RW_EXPORT __declspec(dllimport)
#  endif
#else
#  define OSGVERSE_RW_EXPORT
#endif

namespace osgVerse
{
    struct GraphicsWindowHandle : public osg::Referenced
    {
        void* nativeHandle;  // SDL_Window / GLFWwindow
        void* eglDisplay;    // EGLDisplay
        void* eglSurface;    // EGLSurface
        void* eglContext;    // EGLContext
        GraphicsWindowHandle() : nativeHandle(NULL), eglDisplay(NULL), eglSurface(NULL), eglContext(NULL) {}
    };
}

#endif
