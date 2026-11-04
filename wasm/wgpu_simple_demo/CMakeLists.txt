#
# osgVerse's smallest wasm demo, rendered with WebGPU instead of WebGL 2.
#
# This is a standalone project on purpose: a WebGPU build links different
# libraries than the WebGL 2 build configured by osgverse/CMakeLists.txt (every
# gl*() call has to resolve to gles-on-webgpu's libGLESv2_EGL.a, and Emscripten's
# GL emulation switches -sFULL_ES3 / -sMIN_WEBGL_VERSION must not be passed), so
# it is configured and built by ../Setup.sh WEBGPU (or Setup.bat WEBGPU) after the
# osgVerse libraries are ready.
#
# The WebGPU windowing system (osgVerse::GraphicsWindowWGPU) is part of
# osgVerseReaderWriter and is only compiled when -DVERSE_USE_WEBGPU=ON is used
# for the library build; see readerwriter/CMakeLists.txt.
#
# Inputs (all passed by Setup.sh):
#   ANGLE_WASM_ROOT  gles-on-webgpu wasm SDK (include/ + lib/*.a)
#   OSG_SDK          OpenSceneGraph installation built against that SDK
#   VERSE_BUILD      osgVerse build tree (contains lib/libosgVerse*.a)
#   OSGVERSE_SRC     osgVerse source tree (for the pipeline/ headers)
#   THIRDPARTY_WASM  freetype / jpeg / png / zlib built for wasm
#

CMAKE_MINIMUM_REQUIRED(VERSION 3.13)
PROJECT(osgverse_wgpu_simple_demo LANGUAGES C CXX)

SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

FOREACH(_var ANGLE_WASM_ROOT OSG_SDK VERSE_BUILD OSGVERSE_SRC THIRDPARTY_WASM)
    IF(NOT ${_var})
        MESSAGE(FATAL_ERROR "[wgpu_simple_demo] ${_var} must be passed by Setup.sh")
    ENDIF()
ENDFOREACH()

IF(NOT EMSCRIPTEN)
    MESSAGE(FATAL_ERROR "[wgpu_simple_demo] this project is meant to be built with the Emscripten toolchain")
ENDIF()

SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
SET(CMAKE_EXECUTABLE_SUFFIX ".html")

SET(CANVAS_WIDTH 800)
SET(CANVAS_HEIGHT 600)
CONFIGURE_FILE(shell.html.in "${CMAKE_CURRENT_BINARY_DIR}/shell.html" @ONLY)

# The link order of gles-on-webgpu's archives; wasm-ld does not implement
# $<LINK_GROUP:RESCAN>, and the archives reference each other circularly, so the
# list is repeated on the link line below.
SET(_angle_libs
    "${ANGLE_WASM_ROOT}/lib/libGLESv2_EGL.a"
    "${ANGLE_WASM_ROOT}/lib/liblibANGLE.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_wgpu_backend.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_translator.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_preprocessor.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_common_shader_state.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_image_util.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_gpu_info_util.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_version_info.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_gl_enum_utils.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_frame_capture_mock.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_common.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_compression_utils.a"
    "${ANGLE_WASM_ROOT}/lib/libangle_zlib.a")

SET(_osg_libs
    "${OSG_SDK}/lib/libosgViewer.a"
    "${OSG_SDK}/lib/libosgGA.a"
    "${OSG_SDK}/lib/libosgUtil.a"
    "${OSG_SDK}/lib/libosgDB.a"
    "${OSG_SDK}/lib/libosgText.a"
    "${OSG_SDK}/lib/libosgSim.a"
    "${OSG_SDK}/lib/libosgTerrain.a"
    "${OSG_SDK}/lib/libosg.a"
    "${OSG_SDK}/lib/libOpenThreads.a")

SET(_thirdparty_libs
    "${THIRDPARTY_WASM}/freetype/libfreetype.a"
    "${THIRDPARTY_WASM}/jpeg/libjpeg.a"
    "${THIRDPARTY_WASM}/png/libpng.a"
    "${THIRDPARTY_WASM}/zlib/libzlib.a")

SET(_verse_libs
    "${VERSE_BUILD}/lib/libosgVersePipeline.a"
    "${VERSE_BUILD}/lib/libosgVerseReaderWriter.a"
    "${VERSE_BUILD}/lib/libosgVerseAnimation.a"
    "${VERSE_BUILD}/lib/libosgVerseModeling.a"
    "${VERSE_BUILD}/lib/libosgVerseScript.a"
    "${VERSE_BUILD}/lib/libosgVerseWrappers.a"
    "${VERSE_BUILD}/lib/libosgVerseUI.a"
    "${VERSE_BUILD}/lib/libosgVerseAI.a"
    "${VERSE_BUILD}/lib/libosgVerseDependency.a")

ADD_EXECUTABLE(osgVerse_WebGPUSimple
    webgpu_simple.cpp)

# USE_OSG_PLUGINS() only expands when OSG_LIBRARY_STATIC is defined, and the
# plug-ins have to be linked in by hand because a wasm build has no dlopen().
# osgVerse reads its GLSL modules through osgDB (the 'glsl' reader), so this is
# what makes /assets/shaders work at run time.
FILE(GLOB _osg_plugin_dirs "${OSG_SDK}/lib/osgPlugins-*")
LIST(GET _osg_plugin_dirs 0 _osg_plugin_dir)
SET(_osg_static_plugins "")
FOREACH(_plugin glsl trans rot scale osg rgb bmp
                deprecated_osg deprecated_osgsim deprecated_osgtext
                deprecated_osgviewer deprecated_osgterrain
                serializers_osg serializers_osgtext serializers_osgterrain
                serializers_osgsim)
    LIST(APPEND _osg_static_plugins "${_osg_plugin_dir}/libosgdb_${_plugin}.a")
ENDFOREACH()

# VERSE_NO_NATIVE_WINDOW keeps USE_OSG_PLUGINS() from asking for USE_GRAPHICSWINDOW()
# (OSG's X11 windowing system is not part of this build; the window is ours).
TARGET_COMPILE_DEFINITIONS(osgVerse_WebGPUSimple PRIVATE
                           OSG_LIBRARY_STATIC VERSE_NO_NATIVE_WINDOW)

TARGET_INCLUDE_DIRECTORIES(osgVerse_WebGPUSimple PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ANGLE_WASM_ROOT}/include"
    "${OSG_SDK}/include"
    "${OSGVERSE_SRC}"
    "${OSGVERSE_SRC}/helpers/toolchain_builder/freetype/include"
    "${OSGVERSE_SRC}/helpers/toolchain_builder/jpeg"
    "${OSGVERSE_SRC}/helpers/toolchain_builder/png"
    "${OSGVERSE_SRC}/helpers/toolchain_builder/zlib")

TARGET_COMPILE_OPTIONS(osgVerse_WebGPUSimple PRIVATE -pthread)

# The libraries are listed dependent-first and the cycle-carrying groups twice
# (wasm-ld has no link groups). Everything is static at this point.
TARGET_LINK_LIBRARIES(osgVerse_WebGPUSimple PRIVATE
    ${_verse_libs}
    ${_osg_libs}
    ${_thirdparty_libs}
    ${_angle_libs}
    ${_verse_libs}
    ${_osg_libs}
    ${_thirdparty_libs}
    ${_angle_libs}
    ${_osg_static_plugins})

# WebGPU comes from the browser through the emdawnwebgpu port. ASYNCIFY is what
# lets gles-on-webgpu's blocking waits suspend and resume; without it every
# wgpuInstanceWaitAny() would deadlock the page. -pthread is kept because the OSG
# and osgVerse libraries are compiled with it (serve the page with
# wasm/run_webserver.py so SharedArrayBuffer is available).
TARGET_LINK_OPTIONS(osgVerse_WebGPUSimple PRIVATE
    "--shell-file=${CMAKE_CURRENT_BINARY_DIR}/shell.html"
    # osgVerse loads its GLSL modules from /assets/shaders at run time.
    "--preload-file=${OSGVERSE_SRC}/assets/shaders@assets/shaders"
    "--use-port=emdawnwebgpu"
    "-sASYNCIFY"
    "-sALLOW_MEMORY_GROWTH"
    "-sINITIAL_MEMORY=268435456"
    "-sSTACK_SIZE=8388608"
    "-sENVIRONMENT=web"
    "-sWASM_BIGINT"
    "-sUSE_SDL=2"
    "-pthread"
    "-sPTHREAD_POOL_SIZE=0")

SET_PROPERTY(TARGET osgVerse_WebGPUSimple APPEND PROPERTY LINK_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/shell.html.in")

# Optional model instead of the triangle:
#   -DWEBGPU_SIMPLE_MODEL="http://host/path/model.glb"
IF(WEBGPU_SIMPLE_MODEL)
    TARGET_COMPILE_DEFINITIONS(osgVerse_WebGPUSimple PRIVATE
                               WEBGPU_SIMPLE_MODEL="${WEBGPU_SIMPLE_MODEL}")
ENDIF()

ADD_CUSTOM_COMMAND(TARGET osgVerse_WebGPUSimple POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "[wgpu_simple_demo] ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/osgVerse_WebGPUSimple.html"
    VERBATIM)
