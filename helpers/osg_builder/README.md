# Build scripts for platforms
osgVerse depends on OpenSceneGraph (OSG), so developers should first compile OSG with different GL/GLES options. A few common combinations of options to control OSG CMake results are listed as below.
For OSG 3.6.5, third-party includes (like Google Angle GL/GLES subfolders) should be copied to %osg_build_path%/include first.

#### OSG for GL3/4 Core profile
* CMake options: (not in cmake-gui)
  * OPENGL_PROFILE: GL3 or GLCORE
* You will have to find glcorearb.h from Khronos website. And then set the GLCORE_ROOT environment variable.
* Command-line example: (Windows)
  * <em>cmake -G"Visual Studio 16 2019" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=%sdk_path% -DOPENGL_PROFILE=GLCORE -DGLCORE_INCLUDE_DIR=%glcore_path% "%osg_root_path%"</em>
  * <em>cmake --build .</em>

#### OSG for GLES2/GLES3 (Desktop / GoogleAngle)
* CMake options: (not in cmake-gui)
  * OPENGL_PROFILE: GLES2/GLES3
  * EGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * EGL_LIBRARY: `<GoogleAngle_SDK>`/lib/libEGL.lib
  * OPENGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * OPENGL_gl_LIBRARY: `<GoogleAngle_SDK>`/lib/libGLESv2.lib
* You will have to find include-files and libraries from PowerVR / GoogleAngle SDK. Only support OSG 3.7.0 or later.
  * A quick guild to compile GoogleAngle on Windows/MacOSX
    * Prepare Ninja, Python3 and CMake first. (from Homebrew on MacOSX)
    * <em>git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git</em>
    * <em>git clone https://chromium.googlesource.com/angle/angle</em>
    * <em>set PATH=`<path>`/depot_tools;%PATH% 'OR' export PATH=`<path>`/depot_tools:$PATH</em>
    * <em>set DEPOT_TOOLS_WIN_TOOLCHAIN=0</em>  '''Disable private downloading from Google Cloud
    * <em>cd `<path>`/angle; python.exe scripts/bootstrap.py</em>
    * <em>gclient sync</em>  '''Make sure you can visit the website!
    * <em>gn gen out/Release</em>  '''Generate makefiles
    * Set options in args.gn</em> (like: angle_enable_vulkan = true, angle_enable_metal = true, is_debug = false, etc.)
    * <em>cd out/Release; ninja -j4 -k1 -C out/Release</em>
    * Don't forget to copy vulkan-1.dll as well as libEGL.dll and libGLESv2.dll to executable folder.
* Command-line example: (Windows only)
  * <em>cmake -G"Visual Studio 16 2019" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=%sdk_path% -DOPENGL_PROFILE=GLES3 -DEGL_INCLUDE_DIR=%angle_path%/include -DOPENGL_INCLUDE_DIR=%angle_path%/include -DEGL_LIBRARY=%angle_path%/lib/libEGL.lib -DOPENGL_gl_LIBRARY=%angle_path%/lib/libGLESv2.lib -DOSG_WINDOWING_SYSTEM="None" "%osg_root_path%"</em>
  * <em>cmake --build .</em>
  * osgVerse must be built with -DVERSE_BUILD_WITH_ANGLE=ON

#### OSG for GLES2/GLES3 (MacOSX > 14 / GoogleAngle)
* CMake options: (not in cmake-gui)
  * OPENGL_PROFILE: GLES2/GLES3
  * EGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * EGL_LIBRARY: `<GoogleAngle_SDK>`/lib/libEGL.dylib
  * OPENGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * OPENGL_gl_LIBRARY: `<GoogleAngle_SDK>`/lib/libGLESv2.dylib
  * OPENGL_HEADER2: "#include `<GLES3/gl3.h>`"
* You will have to find include-files and libraries from GoogleAngle 
  * args.gn for compiling GoogleAngle on MacOSX:
    angle_enable_metal = true
    angle_enable_glsl = true
  * Copy Angle include folders to %osg_build_path%/include
  * Remove avfoundation plugin in osgPlugins/CMakeLsits.txt
  * <cm>FIXME: Current OSG cmake has some Apple-related code that force setting OpenGL libraries and include paths.
        (Satrting from line 240 in `<osg_root_path>`/CMakeLists.txt) Will have to modify or remove them manually to
        make Googla Angle (GLES) work. Make these modifications automatically later?</cm>
* Command-line example:
  * <em>cmake -DCMAKE_INSTALL_PREFIX=%sdk_path% -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENGL_PROFILE=GLES3 -DEGL_INCLUDE_DIR=%angle_path%/include -DOPENGL_INCLUDE_DIR=%angle_path%/include -DOPENGL_HEADER2="#include `<GLES3/gl3.h>`" -DEGL_LIBRARY=%angle_path%/lib/libEGL.lib -DOPENGL_gl_LIBRARY=%angle_path%/lib/libGLESv2.lib -DOSG_WINDOWING_SYSTEM="None" "%osg_root_path%"</em>
  * <em>cmake --build .</em>
  * osgVerse must be built with -DVERSE_BUILD_WITH_ANGLE=ON

#### OSG for GLES2/GLES3 (UWP / GoogleAngle)
* CMake options: (not in cmake-gui)
  * CMAKE_SYSTEM_NAME: WindowsStore
  * CMAKE_SYSTEM_VERSION: "10.0"
  * OPENGL_PROFILE: GLES2
  * EGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * EGL_LIBRARY: `<GoogleAngle_SDK>`/lib/libEGL.lib
  * OPENGL_INCLUDE_DIR: `<GoogleAngle_SDK>`/include
  * OPENGL_gl_LIBRARY: `<GoogleAngle_SDK>`/lib/libGLESv2.lib
  * OSG_USE_UTF8_FILENAME: ON
  * OSG_WINDOWING_SYSTEM: "None"
  * _OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE: 0
* You will have to find include-files and libraries from Windows NuGet package.
  * First download Windows Store SDK or latest Windows 11 SDK (with VS2022).
  * Download Angle for UWP: https://www.nuget.org/packages/ANGLE.WindowsStore
  * Rename the .nuget file to .zip and extract it. Find libraries and include files there.
* Patches to version 3.6.5
  * GLES3/gl2.h and GLES3/gl3.h
    * `<Line 44 Insertion>` #define GL_GLEXT_PROTOTYPES
  * src/OpenThreads/win32/Win32Thread.cpp
    * `<Line 434>` //return TerminateThread(pd->tid.get(),(DWORD)-1);
  * src/osgDB/FileNameUtils.cpp
    * `<Line 310>` memcpy(tempbuf1, retbuf, _countof(retbuf));
    * `<Line 320>` if (0 == memcpy(tempbuf1, convertUTF8toUTF16(FilePath).c_str(), convertUTF8toUTF16(FilePath).size()))
  * src/osgDB/FilUtils.cpp
    * `<Line 19>` //typedef char TCHAR;
    * `<Line 247>` if (_wgetcwd(rootdir, MAX_PATH - 1))
      `<Line 249>` return OSGDB_FILENAME_TO_STRING(rootdir);
    * `<Line 861>` retval = OSGDB_WINDOWS_FUNCT(GetSystemWindowsDirectory)(windowsDir, (UINT)size);
* Command-line example: (Windows only)
  * <em>cmake -G"Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION="10.0" -DCMAKE_INSTALL_PREFIX=%sdk_path% -DOPENGL_PROFILE=GLES2 -DEGL_INCLUDE_DIR=%angle_path%/include -DOPENGL_INCLUDE_DIR=%angle_path%/include -DEGL_LIBRARY=%angle_path%/lib/libEGL.lib -DOPENGL_gl_LIBRARY=%angle_path%/lib/libGLESv2.lib -DOSG_USE_UTF8_FILENAME=ON -DOSG_WINDOWING_SYSTEM="None" -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE=0 "%osg_root_path%"</em>
  * Run <em>Solution File</em> and build
  * osgVerse must be built with -DVERSE_BUILD_WITH_ANGLE=ON

#### OSG for MinGW (UCRT64)
* Install OSG:
  * <em>pacman -S mingw-w64-ucrt-x86_64-OpenSceneGraph</em>
* Command-line example:
  * <em>cmake -G"MSYS Makefiles" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release "%osg_root_path%"</em>
  * <em>make</em>

#### OSG for Android (Cross-compiling)
* CMake options: (not in cmake-gui)
  * CMAKE_TOOLCHAIN_FILE: android.toolchain.cmake
  * CMAKE_BUILD_TYPE: Release
  * ANDROID_NDK: `<Android NDK path>`
  * ANDROID_ABI: armeabi/armeabi-v7a/armeabi-v7a with NEON/arm64-v8a/mips/mips64/x86/x86_64
  * ANDROID_NATIVE_API_LEVEL: 21 (find a version in `<Android NDK path>`/platforms)
  * CMAKE_MAKE_PROGRAM (Windows only): `<Android NDK path>`/prebuilt/windows-x86_64/bin/make.exe
  * DYNAMIC_OPENSCENEGRAPH: OFF
  * DYNAMIC_OPENTHREADS: OFF
  * OPENGL_PROFILE: GLES2/GLES3
* Command-line example: (Windows only)
  * <em>cmake -G"MinGW Makefiles" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE="%ndk_path%/build/cmake/android.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DANDROID_ABI="armeabi-v7a with NEON" -DCMAKE_MAKE_PROGRAM="%ndk_path%/prebuilt/windows-x86_64/bin/make.exe" -DANDROID_PLATFORM=21 -DDYNAMIC_OPENSCENEGRAPH=OFF -DDYNAMIC_OPENTHREADS=OFF -DOPENGL_PROFILE=GLES2 "%osg_root_path%"</em>
  * <em>cmake --build .</em>

#### OSG for WASM (Emscripten)
* CMake options: (not in cmake-gui)
  * EGL_LIBRARY: "GL"
  * OSG_GL1_AVAILABLE: OFF
  * OSG_GL2_AVAILABLE: OFF
  * OSG_GLES2_AVAILABLE: ON
  * OSG_WINDOWING_SYSTEM: "None"
  * DYNAMIC_OPENSCENEGRAPH: OFF
  * DYNAMIC_OPENTHREADS: OFF
  * BUILD_OSG_APPLICATIONS: NO
  * _OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE: 0
* Patches to version 3.6.5
  * src/osgUtil/tristripper/include/detail/graph_array.h
    * `<Line 449>` std::for_each(G.begin(), G.end(), std::mem_fn(&graph_array<N>::node::unmark));
  * src/osgPlugins/cfg (Comment out it in src/osgPlugins/CMakeLists.txt)
* Download and prepare the emscripten toolchain, for Linux / WSL only at present:
  * Download emsdk first (e.g. https://github.com/emscripten-core/emsdk/releases/)
  * Configure emsdk:
    * <em>./emsdk update</em>
    * <em>./emsdk install latest</em>
    * <em>./emsdk activate latest</em>
    * <em>source ./emsdk_env.sh</em>
  * <em>cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE=%emsdk_path%/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake %osg_root_path%/examples/osgemscripten</em>
  * <em>make</em>  '''This is the deprecated way to compile OSG to WASM, see osgVerse for a better one!
* Run it in web browsers
  * Copy osgemscripten.html/js/wasm/data to folder
  * Start a simple HTTP server: python -m http.server
  * View 127.0.0.1:8000 in browser

#### OSG for WASM (Emscripten + WebGPU, experimental)
* The GLES3 entry points come from gles-on-webgpu (an ANGLE fork whose back-end renders with the
  browser's WebGPU) instead of Emscripten's WebGL 2 emulation. No GL emulation switch
  (`-sFULL_ES3`, `-sMIN_WEBGL_VERSION`, `-sMAX_WEBGL_VERSION`) may be used, or Emscripten's libGL
  would shadow the ANGLE symbols at link time.
* CMake options: (not in cmake-gui)
  * OPENGL_PROFILE: GLES3
  * OSG_GL1_AVAILABLE / OSG_GL2_AVAILABLE / OSG_GL3_AVAILABLE / OSG_GLES2_AVAILABLE: OFF
  * OSG_GLES3_AVAILABLE: ON
  * OSG_WINDOWING_SYSTEM: "None"
  * DYNAMIC_OPENSCENEGRAPH: OFF
  * DYNAMIC_OPENTHREADS: OFF
  * BUILD_OSG_APPLICATIONS: NO
  * _OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE: 0
  * ANGLE_INCLUDE_DIR: `<angle_wasm_sdk>`/include
  * OPENGL_INCLUDE_DIR / EGL_INCLUDE_DIR: `<angle_wasm_sdk>`/include
  * OPENGL_gl_LIBRARY / EGL_LIBRARY: `<angle_wasm_sdk>`/lib/libGLESv2_EGL.a
* Prepare the gles-on-webgpu wasm SDK first (https://github.com/xarray/gles-on-webgpu, compiled
  with -pthread because OSG and osgVerse are):
  ```bash
  cd <gles_on_webgpu_path>
  source <emsdk_path>/emsdk_env.sh

  emcmake cmake -S . -B out-wasm-pthread \
      -DCMAKE_BUILD_TYPE=Release \
      -DGLES_ON_WEBGPU_PLATFORM=wasm \
      -DGLES_ON_WEBGPU_WASM_PTHREAD=ON
  cmake --build out-wasm-pthread -j"$(nproc)"

  # Pack the SDK used by osgVerse: headers + 14 static libraries
  SDK=$PWD/out-wasm-pthread/angle-wasm-sdk
  rm -rf "$SDK" && mkdir -p "$SDK/include" "$SDK/lib"
  cp -a include/EGL include/GLES include/GLES2 include/GLES3 include/KHR "$SDK/include/"
  cp out-wasm-pthread/*.a "$SDK/lib/"
  ```
* Recommended way: let the setup script do everything (third-party libs, OSG, osgVerse and the
  `wasm/wgpu_simple_demo` demo):
  ```bash
  # Linux / WSL
  cd <osgverse_path>
  ./Setup.sh WEBGPU <emsdk_path> <gles_on_webgpu_path>/out-wasm-pthread/angle-wasm-sdk

  # Windows (run emsdk_env.bat first, ANGLE wasm SDK as 2nd argument)
  Setup.bat WEBGPU <gles_on_webgpu_path>\out-wasm-pthread\angle-wasm-sdk
  ```
  * Intermediate files: `build/3rdparty_wasm`, `build/osg_webgpu`, `build/verse_wasm_webgpu`
  * OSG installation: `build/sdk_wasm_webgpu`
  * Demo page: `build/wgpu_simple_wasm/out/osgVerse_WebGPUSimple.html`
* Manual OSG build with this builder directory:
  * <em>cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=`<emsdk_path>`/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake -DCMAKE_INSTALL_PREFIX=`<sdk_path>` -DOSG_SOURCE_DIR=`<osg_root_path>` -DOSG_BUILD_DIR=`<build_path>`/osg -DANGLE_INCLUDE_DIR=`<angle_wasm_sdk>`/include -DOPENGL_INCLUDE_DIR=`<angle_wasm_sdk>`/include -DEGL_INCLUDE_DIR=`<angle_wasm_sdk>`/include -DOPENGL_gl_LIBRARY=`<angle_wasm_sdk>`/lib/libGLESv2_EGL.a -DEGL_LIBRARY=`<angle_wasm_sdk>`/lib/libGLESv2_EGL.a `<osgverse_path>`/helpers/osg_builder/webgpu</em>
  * <em>cmake --build . -j8 --target install</em>
  * osgVerse must then be built with `-DVERSE_USE_WEBGPU=ON` (this compiles
    `readerwriter/GraphicsWindowWGPU.cpp` into osgVerseReaderWriter) together with the same
    ANGLE include/library variables; see `Setup.sh` mode `WEBGPU` for the full command line.
* Run it in web browsers
  * Serve `build/wgpu_simple_wasm/out` with a COOP/COEP server, because `-pthread` needs
    SharedArrayBuffer: <em>python3 wasm/run_webserver.py</em>
  * Open <em>http://127.0.0.1:8000/osgVerse_WebGPUSimple.html</em> in a WebGPU-capable browser
    (https:// or http://localhost is a secure context, which WebGPU requires)