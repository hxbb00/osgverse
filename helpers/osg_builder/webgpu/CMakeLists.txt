#
# OpenSceneGraph for Emscripten, with GLES 3 served by gles-on-webgpu.
# The 'gles-on-webgpu' repo:
#      https://gitee.com/xarray/gles-on-webgpu
#      https://github.com/xarray/gles-on-webgpu
#
# Adapted from osgverse/helpers/osg_builder/wasm2/CMakeLists.txt. The difference
# is where the GL entry points come from:
#
#   wasm2  - Emscripten's WebGL 2 emulation (MIN_WEBGL_VERSION / FULL_ES3),
#            i.e. every gl*() call is forwarded to a WebGL2RenderingContext.
#   webgpu - the GLES/EGL front-end of gles-on-webgpu, whose WebGPU back-end
#            talks to the browser through the emdawnwebgpu port. No GL
#            emulation flags may be used here, or Emscripten's libGL would
#            shadow the ANGLE symbols at link time.
#
# Required input variables (passed by Setup.sh in the WEBGPU mode):
#   OSG_SOURCE_DIR      OpenSceneGraph source tree
#   OSG_BUILD_DIR       where the OSG targets are generated
#   ANGLE_INCLUDE_DIR   gles-on-webgpu public headers (EGL/, GLES3/, KHR/)
#

CMAKE_MINIMUM_REQUIRED(VERSION 3.13 FATAL_ERROR)
SET(CMAKE_CXX_STANDARD 14)

IF(NOT OSG_SOURCE_DIR OR NOT EXISTS "${OSG_SOURCE_DIR}/CMakeLists.txt")
    MESSAGE(FATAL_ERROR "[osgverse-webgpu] OSG_SOURCE_DIR is not a source tree: '${OSG_SOURCE_DIR}'")
ENDIF()
IF(NOT ANGLE_INCLUDE_DIR OR NOT EXISTS "${ANGLE_INCLUDE_DIR}/GLES3/gl3.h")
    MESSAGE(FATAL_ERROR "[osgverse-webgpu] ANGLE_INCLUDE_DIR has no GLES3/gl3.h: '${ANGLE_INCLUDE_DIR}'")
ENDIF()

# GLES 3 only, no windowing system of its own: osgViewer gets its context from
# the embedding application (the canvas surface created over EGL).
SET(BUILD_OSG_APPLICATIONS NO CACHE BOOL "Do not build applications")
SET(OPENGL_PROFILE "GLES3" CACHE STRING "gles-on-webgpu implements OpenGL ES 3")
SET(OSG_GL1_AVAILABLE OFF CACHE BOOL "Unavailable")
SET(OSG_GL2_AVAILABLE OFF CACHE BOOL "Unavailable")
SET(OSG_GL3_AVAILABLE OFF CACHE BOOL "Unavailable")
SET(OSG_GLES1_AVAILABLE OFF CACHE BOOL "Unavailable")
SET(OSG_GLES2_AVAILABLE OFF CACHE BOOL "OpenGL ES 3 is what gles-on-webgpu provides")
SET(OSG_GLES3_AVAILABLE ON CACHE BOOL "OpenGL ES 3 is what gles-on-webgpu provides")
SET(OSG_WINDOWING_SYSTEM "None" CACHE STRING "The browser canvas is not an OSG windowing system")
SET(DYNAMIC_OPENTHREADS OFF CACHE BOOL "Link OpenThreads statically")
SET(DYNAMIC_OPENSCENEGRAPH OFF CACHE BOOL "Link OpenSceneGraph statically")

# Prevent CMake configuration error (the atomic try_run cannot execute in wasm).
SET(_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE "0" CACHE STRING "Prevent CMake error")
SET(_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE__TRYRUN_OUTPUT "0" CACHE STRING "Prevent CMake error")

# Make sure <EGL/egl.h>, <GLES3/gl3.h> and <KHR/khrplatform.h> resolve to the
# gles-on-webgpu ones. They are prepended so they win over the copies in the
# Emscripten sysroot.
INCLUDE_DIRECTORIES(BEFORE "${ANGLE_INCLUDE_DIR}")

# -O3 keeps the binary small, which matters on the web. Same threading model as
# the rest of osgVerse's wasm build.
ADD_DEFINITIONS("-O3 -pthread -s USE_SDL=2")

INCLUDE_DIRECTORIES(${OSG_SOURCE_DIR}/include)
INCLUDE_DIRECTORIES(${OSG_BUILD_DIR}/include)
LINK_DIRECTORIES(${OSG_BUILD_DIR}/lib)

FILE(MAKE_DIRECTORY ${OSG_BUILD_DIR})
ADD_SUBDIRECTORY(${OSG_SOURCE_DIR} ${OSG_BUILD_DIR})
