# osgVerse - AI Coding Agent Guide

## Project Overview

**osgVerse** is a complete 3D engine solution based on OpenSceneGraph (OSG). It provides a modern rendering pipeline with PBR (Physically Based Rendering), deferred shading, real-time shadows, and comprehensive 3D functionality including physics, animation, and UI systems.

- **Language**: C++ (C++14/C++17)
- **Build System**: CMake 3.10+
- **License**: See LICENSE file

## Technology Stack

### Core Technologies
- **Graphics API**: OpenGL 2.0+, OpenGL 3.3+ Core Profile, GLES2/GLES3, WebGL 1/2
- **3D Framework**: (Minimum) OpenSceneGraph 3.1.1; (Preferred) OpenSceneGraph 3.6.5
- **Build System**: CMake 3.10+
- **Platforms**: Windows (10-11), Linux (Ubuntu/Debian/Kylin/UOS), macOS, Android, WebAssembly (Emscripten)

### Key Dependencies
- **OpenSceneGraph** (Required): Core 3D framework
- **OpenGL/GLES**: Graphics rendering
- **SDL2**: Windowing for Android/IOS/WASM
- **CUDA/MUSA** (Optional): GPU compute
- **Bullet3** (Optional): Physics simulation
- **Qt5/Qt6** (Optional): Qt-based applications
- **FFmpeg** (Optional): Video decoding/encoding
- **Draco** (Optional): Mesh compression
- **ZLMediaKit** (Optional): Media streaming

### Embedded 3rdparty Libraries (3rdparty/)
The project includes many embedded third-party libraries:
- **thorvg**: 2D vector graphics engine (Drawer2D)
- **imgui**: Immediate mode GUI (with extensions like ImGuizmo, implot)
- **leveldb**: Key-value storage
- **libhv**: High-performance network library
- **ktx**: Khronos texture format
- **ozz**: Animation runtime
- **recastnavigation**: Navigation mesh
- **marl**: Task scheduler
- **meshoptimizer**: Mesh optimization
- **Eigen**: Linear algebra
- And many more (see THIRDPARTY_LICENSES.md for complete list)

Notes for the vendored ThorVG (`3rdparty/thorvg`): only the CPU rasterizer and the
`sfnt`/`raw`/`svg` loaders are included. `thorvg_config.h` is generated from
`thorvg_config.h.in` (the upstream `config.h` include was renamed to avoid clashing with
other 3rdparty `config.h`), `THORVG_AVX_SUPPORT` / `THORVG_NEON_SUPPORT` is enabled when
the host CPU supports it, and the global `operator new/delete` overrides were removed so
ThorVG does not hijack the host allocator.

## Project Structure

```
osgVerse/
├── CMakeLists.txt              # Root CMake configuration
├── Setup.sh / Setup.bat        # Automated build scripts
├── VerseCommon.h               # Main unified header
├── CODE_STYLE.md               # Coding style guidelines
│
├── 3rdparty/                   # Embedded third-party libraries
│   ├── thorvg/                # 2D vector graphics (Drawer2D)
│   ├── imgui/                 # GUI library
│   ├── leveldb/               # Database
│   ├── libhv/                 # Network library
│   ├── ktx/                   # Texture format
│   ├── ozz/                   # Animation
│   └── ...                    # Many more
│
├── pipeline/                   # Rendering pipeline (osgVersePipeline)
│   ├── Pipeline.h/cpp         # Core pipeline
│   ├── DeferredCallback.h/cpp # Deferred shading
│   ├── ShadowModule.h/cpp     # Shadow rendering
│   ├── LightModule.h/cpp      # Lighting system
│   ├── ShaderLibrary.h/cpp    # Shader management
│   └── ...
│
├── modeling/                   # Geometry processing (osgVerseModeling)
│   ├── MeshDeformer.h/cpp     # Mesh deformation
│   ├── GeometryMerger.h/cpp   # Geometry merging
│   ├── Math.h/cpp             # Math utilities
│   └── ...
│
├── readerwriter/              # I/O utilities (osgVerseReaderWriter)
│   ├── DracoProcessor.h/cpp   # Draco compression
│   ├── KTXProcessor.h/cpp     # KTX textures
│   ├── GLTFReader.h/cpp       # GLTF support
│   └── ...
│
├── animation/                  # Animation & physics (osgVerseAnimation)
│   ├── PhysicsEngine.h/cpp    # Bullet physics
│   ├── PlayerAnimation.h/cpp  # Character animation
│   ├── TweenAnimation.h/cpp   # Tweening
│   └── ...
│
├── ui/                         # User interface (osgVerseUI)
│   ├── imgui/                 # ImGui integration
│   ├── Canvas2D.h/cpp         # 2D canvas
│   └── ...
│
├── script/                     # Scripting (osgVerseScript)
│   └── ...
│
├── ai/                         # AI & navigation (osgVerseAI)
│   └── ...
│
├── wrappers/                   # Serialization wrappers (osgVerseWrappers)
│   └── ...
│
├── plugins/                    # OSG plugins
│   ├── osgdb_fbx/             # FBX format
│   ├── osgdb_gltf/            # GLTF format
│   ├── osgdb_ktx/             # KTX format
│   ├── osgdb_3dgs/            # 3D Gaussian Splatting
│   ├── osgdb_ffmpeg/          # Video support
│   └── ...
│
├── tests/                      # Test applications
│   ├── pipeline_test.cpp      # Pipeline testing
│   ├── shadow_test.cpp        # Shadow testing
│   ├── physics_basic_test.cpp # Physics testing
│   └── ...
│
├── applications/               # Main applications
│   ├── viewer/                # Basic viewer
│   ├── viewer_composite/      # Multi-view viewer
│   ├── scene_editor/          # Scene editor
│   ├── earth_explorer/        # Earth visualization
│   ├── qt_viewer/             # Qt integration
│   └── ...
│
├── wasm/                       # WebAssembly specific code
├── android/                    # Android specific code
├── helpers/                    # Build helpers
│   ├── toolchain_builder/     # 3rdparty build tools
│   └── osg_builder/           # OSG build helpers
│
├── assets/                     # Assets (models, shaders, textures)
│   ├── models/                # 3D models
│   ├── shaders/               # GLSL shaders
│   │   ├── deferred/          # StandardPipeline per-frame stages (GBuffer, shadows,
│   │   │                      #   SSAO/SSGI, deferred lighting, post effects: std_*)
│   │   ├── forward/           # Forward path shaders (std_forward_render.*)
│   │   └── *.module.glsl      # Shared include modules (stay in the root on purpose:
│   │                          #   std_common_quad.*, IBL precomputation, skybox...)
│   ├── skyboxes/              # HDR skyboxes
│   └── textures/              # Textures
│
├── cmake/                      # CMake modules
│   ├── VerseMacros.cmake      # Build macros
│   └── ...
│
└── build/                      # Build output (generated)
```

## Module Dependency Chain

| Module | Dependencies | Optional External |
|--------|-------------|-------------------|
| osgVerseDependency | - | - |
| osgVerseModeling | Dependency | cinolib |
| osgVersePipeline | Dependency, Modeling | CUDA |
| osgVerseScript | Dependency, Pipeline | - |
| osgVerseAI | Dependency, Modeling | - |
| osgVerseAnimation | Dependency, Pipeline, Modeling | Bullet, Effekseer |
| osgVerseUI | Dependency, Modeling, Script | libCEF |
| osgVerseReaderWriter | Dependency, Animation, Modeling, Pipeline | libDraco, SDL |
| osgVerseWrappers | ALL | - |

## Build Instructions

### Quick Build (Recommended)

Use the provided setup scripts. The first argument selects the build mode
(`DEFAULT` | `CORE` | `GLES2` | `GLES3` | `WEBGL1` | `WEBGL2` | `ANDROID` |
`WEBGPU`); run without arguments to choose interactively. `WEBGL1/2` also
require the emsdk path as the second argument; `WEBGPU` (experimental, GLES 3
over WebGPU through gles-on-webgpu) requires the emsdk path and the
gles-on-webgpu wasm SDK (`include/` + `lib/*.a`) as the second and third ones.

**Windows:**
```cmd
Setup.bat DEFAULT       :: Desktop OpenGL -> build/verse_def, install -> build/sdk_def
Setup.bat GLES3         :: OpenGL ES 3   -> build/verse_es,  install -> build/sdk_es
Setup.bat WEBGL2 D:\emsdk   :: WebAssembly  -> build/verse_wasm2, install -> build/sdk_wasm2
Setup.bat WEBGPU D:\angle-wasm-sdk  :: WebGPU -> build/verse_wasm_webgpu, demo -> build/wgpu_simple_wasm/out
```

**Linux/macOS:** same modes with `./Setup.sh DEFAULT` etc.

The script will:
1. Download OpenSceneGraph if not present
2. Build third-party libraries
3. Build OpenSceneGraph
4. Build osgVerse

Each mode keeps its own build tree under `build/`, so multiple modes can be
built side by side (e.g. `verse_def` for desktop, `verse_wasm2` for web).

### Manual Build

```bash
# 1. Ensure OpenSceneGraph is built and OSG_ROOT is set
export OSG_ROOT=/path/to/osg

# 2. Create build directory
mkdir build && cd build

# 3. Configure
cmake .. -DOSG_ROOT=$OSG_ROOT

# 4. Build
cmake --build . --target install --config Release
```

### Key CMake Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `OSG_ROOT` | Path | - | OpenSceneGraph root directory |
| `VERSE_3RDPARTY_PATH` | Path | ../Dependencies | Third-party libraries path |
| `VERSE_BUILD_EXAMPLES` | Bool | ON | Build examples and tests |
| `VERSE_BUILD_WITH_QT` | Bool | ON | Build Qt-based applications |
| `VERSE_BUILD_WITH_CUDA` | Bool | ON | Build CUDA-based libraries |
| `VERSE_STATIC_BUILD` | Bool | OFF | Static library build |
| `VERSE_USE_OSG_STATIC` | Bool | OFF | Use static OSG |
| `VERSE_SUPPORT_CPP17` | Bool | ON | Enable C++17 features |
| `VERSE_BUILD_DEPRECATED_TESTS` | Bool | OFF | Build deprecated tests |

### Platform-Specific Builds

**WebAssembly (Emscripten):**
```bash
./Setup.sh WEBGL2 /path/to/emsdk
```

**Android:**
```bash
# Set ANDROID_SDK and ANDROID_NDK environment variables first
export ANDROID_SDK=/path/to/sdk
export ANDROID_NDK=/path/to/ndk
./Setup.sh ANDROID
```

**OpenGL ES:**
```bash
./Setup.sh GLES3        # or GLES2
```

## Code Style Guidelines

See `CODE_STYLE.md` for full details. Key points:

### Formatting
- 4 spaces indent
- 8 spaces continuation indent  
- 100 columns max
- Opening brace `{` on new line
- Spaces around operators

### Naming Conventions
- **Constants**: UPPER_CASE (e.g., `FOO_COUNT`)
- **Global variables**: `g_` prefix (e.g., `g_globalWarming`)
- **Static/class variables**: `s_` prefix
- **Private/protected attributes**: `_` prefix (e.g., `_attributeName`)
- **Class names**: UpperCamelCase (e.g., `FooBar`)
- **Methods/variables**: lowerCamelCase (e.g., `methodName`)

### File Conventions
- Headers: `.h`
- Implementation: `.cpp`
- Inline includes: `.inline`
- Public headers: `#include <module/Header.h>`
- Private headers: `#include "Header.h"`

### Example:
```cpp
class FooBar
{
public:
    void methodName(int arg1, bool arg2);
    int sizeInBytes;
    
private:
    int _attributeName;
    static int s_globalAttribute;
    enum { ONE, TWO, THREE };
};
```

## Testing

### Test Organization
Tests are in `tests/` directory and categorized as:

**Regular Tests (NEW_TEST):**
- `osgVerse_Test_Compressing` - KTX/Draco compression
- `osgVerse_Test_Thread` - Marl task scheduler
- `osgVerse_Test_Volume_Rendering` - Volume rendering methods
- `osgVerse_Test_Auto_LOD` - Geometry merging/optimization
- `osgVerse_Test_Sky_Box` - Skybox and atmosphere

**Examples (NEW_EXAMPLE):**
- `osgVerse_Test_Pipeline` - Basic pipeline usage
- `osgVerse_Test_Shadow` - Shadow algorithms
- `osgVerse_Test_Forward_Pbr` - Forward PBR rendering
- `osgVerse_Test_ImGui` - ImGui integration
- `osgVerse_Test_Earth` - Earth/atmosphere/ocean
- `osgVerse_Test_Perf_Profile` - Per-model GPU/counter/resource profiling with HUD
- `osgVerse_Test_Physics_Basic` - Physics simulation (requires Bullet)
- And many more...

### Running Tests
Tests are built when `VERSE_BUILD_EXAMPLES=ON` (default). In each mode's
build tree (e.g. `build/verse_def`), binaries land in `bin/` and are also
installed to `build/sdk_<mode>/bin`. To build and run a single test:

```bash
cd build/verse_def                      # or the mode you configured
cmake --build . --target osgVerse_Test_Pipeline --config Release
bin/osgVerse_Test_Pipeline              # Windows: bin\osgVerse_Test_Pipeline.exe
```

Common usage (see each test's own `arguments.read(...)` calls for the full list):
```bash
bin/osgVerse_Test_Pipeline --screen 1              # select monitor for multi-screen
bin/osgVerse_Test_Pipeline --with-history          # enable history buffer pass
bin/osgVerse_Test_Pipeline --user-module-pre       # add a pre-final-stage UI pass
bin/osgVerse_Test_Pipeline --openxr                # XR rendering
```

Note: `osgVerse_Test_Physics_Basic` etc. only build when their optional deps
(Bullet/FFmpeg/Draco) are found; missing targets usually mean a dep was not
detected, not a build failure.

### Verifying Rendering Changes (Agent Workflow)

Rendering work must be **run and inspected**, not only compiled: the pipeline is
multi-pass, feature-flag driven and version dependent, and most regressions (state leaks,
wrong buffers, flicker) only show up at draw time. The following workflow is what an agent
should use, in order.

**1. Build *and install* the target for the mode under test.** Each mode owns a build tree
and an SDK prefix, so a library edit is only visible after `install`:

```bash
# core (desktop GL3):  build/verse_core -> build/sdk_core
cmake --build build/verse_core --target osgVerse_Viewer -j8
cmake --build build/verse_core --target install -j8
# compat (desktop GL2): build/verse_def  -> build/sdk
cmake --build build/verse_def  --target osgVerse_Viewer -j8
cmake --build build/verse_def  --target install -j8
```

Run the installed binary from `build/sdk_<mode>/bin` (RPATH is `$ORIGIN:$ORIGIN/..:
$ORIGIN/../lib`). Check `CMAKE_INSTALL_PREFIX` in the mode's `CMakeCache.txt` if unsure.

**2. Drive the viewer head-lessly-ish and capture images.** `configureViewerForAgent()`
(`pipeline/Utilities.h`, impl `pipeline/UtilitiesEx.cpp`) turns any test or the viewer into
a scriptable case: it renders N frames, presses optional keys, writes PNGs and quits. It is
already wired into `viewer`, `pipeline_test`, `shadow_test`, `many_lights_test` and
`forward_pbr_test`:

```bash
cd build/sdk_core/bin
./osgVerse_Viewer --agent --agent-frames 90 --agent-size 1280 720 \
    --agent-eye 0 0 120 --agent-center 0 0 0 \
    --agent-keys "c" --agent-output /tmp/shot
# -> /tmp/shot_<index>.png, then the process exits
```

Options: `--agent`/`--ai` enable it; `--agent-frames N` (default 60); `--agent-output PREFIX`;
`--agent-fps N` (`0` = every frame); `--agent-eye/--agent-center/--agent-up X Y Z`;
`--agent-size W H`; `--agent-keys "c,s"` (single chars or numeric key codes, pressed a couple
of frames before the capture so their effect is in the image); `--agent-keep` (don't quit).
`--agent-keys` is how you A/B a feature in one run: e.g. `many_lights_test` toggles light
clustering with `[C]`, animation with `[A]`, light wireframe with `[L]`.

For a plain `timeout N ./osgVerse_Viewer` run (no `--agent`), the window is still created on
the machine's display; redirect the log to a file and inspect it as below.

**3. Turn the log into a pass/fail signal.** Rendering bugs surface as repeated driver
messages long before they are visible. Count and classify them:

```bash
grep -c  "invalid enumerant"        /tmp/viewer.log   # must NOT scale with frames
grep -oE "after applying GLMode 0x[0-9a-f]+" /tmp/viewer.log | sort | uniq -c
grep -n  "AlphaFunc::apply\|Invalid primitive mode\|not supported" /tmp/viewer.log
```

Compare against a case known to be clean (e.g. `osgVerse_Test_Many_Lights`) to separate
pre-existing noise from a regression: a handful of notices on the first realized frames is
normal; a count that grows every frame is the bug.

**4. Localize a GL error to the call that produced it.** OSG's default is
`ONCE_PER_FRAME`; set `OSG_GL_ERROR_CHECKING=ONCE_PER_ATTRIBUTE` to make it report the
attribute/mode right after which the error was raised:

```bash
OSG_GL_ERROR_CHECKING=ONCE_PER_ATTRIBUTE ./osgVerse_Viewer 2>&1 | grep "invalid enumerant"
# e.g. "... after applying GLMode 0xde1"  -> GL_TEXTURE_2D enable is the culprit
```

**5. Use an `LD_PRELOAD` GL shim when the error is not inside `State::apply`.** Hook
`glEnable`/`glDisable`/`glDrawArrays`/`glDrawElements`/`glTexBuffer`/`glBindBuffer` (and,
if needed, OSG's mangled `osg::State::apply(StateSet*)` / `osg::State::applyMode`) and
print a `backtrace()` when a removed enum is seen. This is what identified the
`GL_QUADS`, `GL_TEXTURE_2D` and `GL_NORMALIZE` sources. `nm -D` the `.so` and keep it in
`/tmp` — it is a diagnostic tool, not part of the build.

**6. Use the engine's own GL debug callback.** `pipeline/PipelineStandard.cpp` sets up
`glDebugMessageCallback` (KHR_debug) in `RealizeOperation` for GL3 builds and prints each
unique message once (`[GLDebug HIGH/MEDIUM/...]`), with a repeat counter to avoid flooding.
If you see no `[GLDebug]` line while `invalid enumerant` still appears, the callback is not
being installed (check that `RealizeOperation` actually runs on the presenting context) —
that is itself a bug worth fixing, as the driver message names the offending call.

**7. Diff images for a visual regression.** Capture the same view with the feature ON and
OFF (`--agent-keys`, `--no-shadows`, `--no-ssao`, `--no-ssgi`, `--no-bloom`,
`--no-cluster`, `--cluster-tile`, `--cluster-slices`, ...), then compare the PNGs
pixel-wise. A non-zero
diff is expected where the feature is visible; a diff *outside* that region is a
regression. `osgVerse::ScreenSnapshotCallback` (`pipeline/Utilities.h`) can be attached
directly to a camera's `FINAL_DRAW` when a test needs custom capture timing.

**8. Confirm the correct camera is being watched.** The main camera is detached in a
pipeline viewer (see "Presentation detail" above); attach snapshot/debug callbacks to the
last slave with a graphics context, or to the specific stage camera
(`pipeline->getStage("Lighting")->camera`), never to `viewer.getCamera()` by default.

## Development Conventions

### Adding a New Library

1. Add source files to appropriate module's `CMakeLists.txt`
2. Use `NEW_LIBRARY()` macro for regular libraries
3. Use `NEW_CUDA_LIBRARY()` for CUDA/MUSA libraries
4. Link dependencies using `TARGET_LINK_LIBRARIES()`

### Adding a New Plugin

1. Create subdirectory in `plugins/`
2. Create `ReaderWriterXXX.cpp` implementing OSG plugin interface
3. Add subdirectory to `plugins/CMakeLists.txt`
4. Use `NEW_PLUGIN()` macro

### Adding a New Test

1. Add `.cpp` file to `tests/`
2. Add `NEW_TEST(exe_name source.cpp)` to `tests/CMakeLists.txt`
3. Or use `NEW_EXAMPLE()` for example applications

### Export Macros

Use export macros for cross-platform compatibility:
```cpp
#include <wrappers/Export.h>
// For pipeline: OSGVERSE_PIPELINE_EXPORT
// For modeling: OSGVERSE_MODELING_EXPORT
// etc.
```

## Key Architecture Patterns

### Pipeline Architecture
The rendering pipeline uses a stage-based architecture:
- `Pipeline` (`pipeline/Pipeline.h/cpp`): Main orchestrator; owns the ordered list of
  `Stage`s, the registered modules, the framebuffer graph and the GL/GLSL version state
- `Pipeline::Stage`: one render pass. `Stage::outputs` is a `name -> osg::Texture` map
  (the buffers it produces), `Stage::getOrCreateStateSet()` gives the state set to set
  its program/inputs on. A `Stage` is normally an `osg::Camera`, but a deferred stage is
  an RTT `RttGeometryRunner` (see below)
- Modules extend behavior: `LightModule`, `ShadowModule`, `UserInputModule`. Each
  registers itself with `pipeline->addModule(name, this)` (names: `"Light"`, `"Shadow"`,
  and the user-input module) and gets `getModule("Light")` / `getModule("Shadow")`
- `StandardPipelineParameters` (`pipeline/Pipeline.h`): config struct passed to
  `setupStandardPipeline()`; carries masks, shadow settings, SSAO/bloom/TAA toggles,
  `enableUserInput`, resolution, etc.

**Stage helpers and typical order.** `setupStandardPipeline()` builds the graph with
`addInputStage()`, `addWorkStage()`, `addDeferredStage()`, `addDisplayStage()` in roughly
this order (exact set depends on the parameters):

```
GBuffer (input, mask = deferredMask)
  -> Ssao -> SsaoBlur1 -> SsaoBlur2            (when enableAO)
  -> Lighting                                   (3 MRT outputs: ColorBuffer +
                                                 IblAmbientBuffer + IblSpecularBuffer)
  -> Shadowing                                  (shadow combine; when shadows are on)
  -> Ssgi -> SsgiBlur1 -> SsgiBlur2            (when enableGI; traces the *shadowed*
                                                 radiance written by Shadowing)
  -> [user "Forward" input stage]               (added by addUserInputStage())
  -> [Brighting -> downsample chain -> combining]  (when enableBloom)
  -> DeferredSky                                (when useDeferredSky / TAA is on)
  -> ToneMapping (composes indirect light, see below)
  -> AntiAliasing (FXAA or TAA) -> Final (display stage)
```

**Indirect-light fallback chain.** The deferred lighting stage keeps diffuse and specular
ambient in *separate* MRT buffers on purpose: screen-space effects can then be composed at
tone mapping time as `indirectDiffuse = ssgi.rgb + (1 - ssgi.a) * iblDiffuse`, where
`ssgi.a` is hemisphere *coverage* (how much of the pixel's hemisphere actually sees other
geometry). Where nothing is traced, the IBL stays — this is the pluggable chain
screen-space → (future: probes/DDGI) → IBL. The exposure (luminance) stage meters the same
radiance the tone mapping sums, including the specular IBL and GI buffers.

**Pipeline masks** decide which pass draws which node — set them with
`Pipeline::setPipelineMask(node, mask)`; several masks can be OR-ed:

| Mask | Value | Meaning |
|------|-------|---------|
| `DEFERRED_SCENE_MASK` | `0xff000000` | opaque geometry fed to the G-buffer |
| `FORWARD_SCENE_MASK` | `0x000000ff` | geometry drawn in forward passes (sky, HUD, 3DGS) |
| `SHADOW_CASTER_MASK` | `0x00100000` | geometry rendered into the shadow maps |
| `CUSTOM_INPUT_MASK` | `0x00010000` | geometry for a custom/user input stage |

**Presentation detail (important when debugging "nothing shows up").** In a pipeline
viewer the *main* camera is detached (`mainCam->setGraphicsContext(NULL)`); the image on
screen is produced by the last `View::Slave` that still owns a graphics context (the
forward pass or the final stage). Anything that must act on the real back buffer — a
snapshot callback, a `glEnable`, a debug hook — has to go on that slave camera, not on
`viewer.getCamera()`. `configureViewerForAgent()` scans the slaves in reverse for exactly
this reason.

**Deferred vs work stages.** Deferred stages use `osgVerse::DeferredRenderCallback`
(`pipeline/DeferredCallback.h/cpp`): their work is done by an `RttGeometryRunner`
geometry, so `Stage::getOrCreateStateSet()` returns the *geometry's* state set and the
stage's outputs are the RTT textures of that runner. Work stages are plain
`osg::Camera`s. `Pipeline::getDeferredCallback()` gives access to the shared callback
(and to `isJitterEnabled()` for TAA).

**Integration pattern (all examples/tests use it):** subclass `osgViewer::Viewer`,
override `createRenderer(camera)` and return `pipeline->createRenderer(camera)`.
Or use `osgVerse::StandardPipelineViewer` (defined in `pipeline/Pipeline.h`,
implemented in `pipeline/PipelineStandard.cpp`) for a ready-made viewer. See
`tests/pipeline_test.cpp` and `tests/shadow_test.cpp`. A `Global.h`
`osgVerse::globalInitialize(argc, argv)` call must run before creating the viewer.

**Shader compatibility layer:** shaders must NOT use raw GLSL keywords. Write them
with the `VERSE_*` macros injected by `ShaderLibrary::createShaderDefinitions()`
(see `pipeline/ShaderLibrary.cpp`): `VERSE_MATRIX_MVP`, `VERSE_TEX1D/2D/3D/CUBE`,
`VERSE_VS_IN/OUT`, `VERSE_FS_IN`, `VERSE_FS_FINAL`, `VERSE_lambertDiffuse`,
`VERSE_blinnPhongSpecular`. This keeps one shader source working on OpenGL,
GLES and WebGL. Without these macros shaders will fail on ES/WASM.

**Static build plugin registration:** with `VERSE_STATIC_BUILD`/`OSG_LIBRARY_STATIC`,
each test/plugin must call `USE_OSG_PLUGINS()`, `USE_VERSE_PLUGINS()`, and
`USE_GRAPICSWINDOW_IMPLEMENTATION(SDL/GLFW)` at the top of `main()` or plugins
won't load. See `tests/pipeline_test.cpp` for the exact `#ifdef` block.

### Light & Shadow Systems

**LightModule + LightCluster (`pipeline/LightModule.*`, `pipeline/LightCluster.*`).** The
light parameter table is uploaded either as an `osg::TextureBuffer` (TBO, read with
`samplerBuffer`/`texelFetch`) or as a legacy `Texture2D`. The default is chosen from the
*runtime* GL/GLSL versions via `isTextureBufferSupported(glVer, glslVer)`
(`glVersion >= 310 && glslVersion >= 140`, `pipeline/Pipeline.h`), so a desktop GL3 core
context **and** a compatibility-profile context on a modern driver both get TBO, while
GLES2/WebGL1 keeps Texture2D. Force it with `LightModule::setUseTextureBuffer()`.
`ShaderLibrary::createShaderDefinitions()` emits the matching `VERSE_LIGHT_BUFFER` /
`VERSE_LIGHT_CLUSTER` defines from the same runtime versions, so the shader path and the
upload path can never disagree.

Clustered light culling is on by default whenever the TBO path is active
(`LightCluster::setEnabled()`, `LightModule::setClusterEnabled()`). It splits the screen
into tiles x logarithmic depth slices (grid size depends on tile pixels and slice count,
e.g. 128 px / 16 slices covers a 1920x1080 view with ~15x9x16 = 2160 clusters). Two
`osg::TextureBuffer`s are produced: a per-entry light-index buffer (`GL_R32F`, read with
`texelFetch`) and a per-cluster `(offset, count)` buffer. Uniforms: `LightNumber`,
`LightClusterEnabled`, `LightClusterGrid`, `LightClusterTile`. `setMaxLightsInPass()` must
cover the total light count (main light + dynamic lights) or the extra lights are dropped.

**ShadowModule (`pipeline/ShadowModule.*`).** Techniques are selected with
`setTechnique()` and **must be set before `createStages()`**: `DefaultSM`, `BandPCF`,
`PossionPCF`, `VSM`, `ESM`, `EVSM` (see `ShadowModule::TechniqueItem`). Several cascades
are packed by view range, and the VSM/EVSM paths add a blur chain the receiver-side combine
shader consumes. `shadowNumber`, `shadowResolution` and the bias/exponent parameters come
from `StandardPipelineParameters`.

### Screen-Space Lighting Effects: SSAO / SSGI / Exposure

**SSAO (`deferred/std_ssao*.frag.glsl`)** produces a hemisphere occlusion factor applied
*only to indirect light* (diffuse uses `min(materialAO, SSAO)`, specular uses the GTSO
function of Jimenez 2016), never to direct light — applying it to direct light double-
darkens shadowed areas.

**SSGI (`deferred/std_ssgi*.frag.glsl`, `enableGI`, default on; GLES2 and
`debugShadowCombination` skip it)** is a one-bounce SSDO-style tracer: 16 golden-angle
spiral samples in a screen-space disk around the pixel; a sample counts if it is above the
horizon, inside the radius and not on the same surface. It returns the sample's own
*outgoing* radiance (read from the **Shadowing** stage's combined buffer, so bounced light
already respects shadows and albedo) times its weight, and accumulates hemisphere
*coverage* in alpha. Output is `rgb = radiance * coverage` (premultiplied so the bilateral
blur never erodes uncovered regions), `a = coverage`. Tone mapping composes
`ssgi.rgb + (1 - ssgi.a) * iblDiffuse + iblSpecular`.

Radius policy: with `GIRadius > 0` it is a world-unit radius (projected at the pixel
depth); with `GIRadius <= 0` (the default) a constant on-screen radius (~40 px, ~5% of
viewport height) is used and the corresponding world radius is derived per pixel — osgVerse
scenes range from centimeter objects to a globe, so no single world radius fits all.

**Next steps planned for the lighting system (see TODO_cn.md for tracking).** The A/B
harness for all of this is `many_lights_test`: keys **1-6** switch at runtime between
forward/deferred x no-lights/many-lights x no-GI/GI (or `--mode N` head-less), so every
plan item can be validated against the same view with one keypress:
1. **Temporal reprojection (prerequisite: velocity buffer).** The screen-space effects are
   noisy at 16 samples and flicker in motion; TAA already has a history chain, but there is
   no per-pixel motion vector buffer yet. Add a `VelocityBuffer` output in the G-buffer
   (previous-frame clip-space position via a previous-view matrix), then give SSAO/SSGI a
   temporal accumulation pass and TAA proper motion handling (removes ghosting too).
2. **Forward output into the post chain.** The forward pass draws to the back buffer after
   the display stage, so forward results skip tone mapping/exposure (visible as blown-out
   highlights in modes 1/2 of `many_lights_test`). Either render the forward scene into a
   buffer that the tone mapping reads, or append a tonemap pass after the forward pass;
   until then, forward modes are for structural comparison only, not photometric A/B.
3. **Half-resolution SSGI + joint bilateral upsample** (upsample with depth+normal guides,
   like the blur) — the tracer is the most expensive full-res pass at the moment.
4. **Second bounce / bent normals**: the Ssgi stage output is already a pluggable slot in
   the fallback chain; a second trace reading the blurred GI buffer (or feeding GI into
   the specular occlusion) can build on it without touching tone mapping again.
5. **Deferred contact shadows** for point lights (screen-space raymarch against the depth
   buffer in the lighting stage) — a cheap win on top of the existing cluster lighting.
6. **Fallback chain level 2 — probe-based GI (DDGI-style)**: replace/augment the IBL
   diffuse fallback for pixels with low screen-space coverage.
7. `standard_pipeline*.json` still embed the old shader sources (2-MRT ambient, RGB
   normals, no GI); they are self-consistent variants but drift further with every shader
   change — either regenerate them from the .glsl files or drop the embedding.

### 3DGS Training (VkSplat, Vulkan)

- Vendored core: `3rdparty/vksplat/` (Apache-2.0, glm replaced by Eigen). It is built as `osgVerseVkSplat` only when `VERSE_BUILD_WITH_VKSPLAT=ON`, Vulkan is found, and the target is desktop Linux/Windows.
- Public API: `modeling/GaussianTrainer.h` (`TrainerInput`, `GaussianTrainerParameters`, `GaussianTrainer`). The implementation is compiled into the separate `osgVerseGaussianTrainer` static library (`modeling/GaussianTrainer.cpp`), so the core modeling library does not depend on Vulkan.
- Dataset IO: `plugins/osgdb_colmap` registers the `colmap` protocol/extension and returns a `TrainerInput`; use `osgDB::readObjectFile("colmap://path")` or `"path.colmap"`. `globalInitialize()` preloads the plugin so the protocol is registered before reading.
- Shaders: `assets/shaders/3dgs_trainer/` (`slang/` sources, `generated/` and `radix_sort/` SPIR-V). They are stored explicitly and loaded from disk at runtime; `compile_shaders.py` in the same folder regenerates the SPIR-V (requires `slangc`/`glslc`).
- The trainer owns its own `VkDevice` and compute queue and never touches the GL context during training. `createGeometry()` performs a one-time readback and builds a `GaussianGeometry` (INSTANCING) for the existing `GaussianGeometry`/`GaussianSorter` path; there is no per-step GL readback and no GL-Vulkan interop.
- Test: `osgVerse_Test_Gaussian_Training` (`tests/gaussian_training_test.cpp`), e.g. `--dataset colmap:///data/garden --steps 200 --no-render`, or with `--agent ...` to capture a rendered image.

### GL Core Profile & Fixed-Function State

A desktop GL core profile (and GLES/WebGL) has **removed** the whole fixed-function
pipeline: the fixed-function texture enables (`GL_TEXTURE_1D/2D/3D/CUBE_MAP`,
`GL_TEXTURE_RECTANGLE`), `GL_NORMALIZE`, `GL_ALPHA_TEST`/`AlphaFunc`, `GL_LIGHTING`,
`GL_FOG`, `GL_COLOR_MATERIAL`, `GL_AUTO_NORMAL`, `GL_TEXTURE_GEN_*`, the smoothing modes
(`GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`, `GL_POLYGON_SMOOTH`) and the legacy `GL_CLIP_PLANE0-5`.
Enabling any of them makes the driver report `invalid enumerant` **for every draw that uses
the state set**, which floods the log (tens of thousands of lines) — and
`osg::AlphaFunc::apply()` even prints `not supported`.

The removal is done **once at load time**, not at draw time, by
`osgVerse::FixedFunctionOptimizer` (`readerwriter/Utilities.h/cpp`): `ModeChecker` keeps
only the modes that are still legal for the current build, and `removeUnusedStateAttributes()`
also drops the fixed-function texture modes, `TEXENV`/`TEXGEN`/`TEXMAT`, `AlphaFunc`,
`Material` (converted to a color array), `Fog`, `Light*`, `ShadeModel`, and fixes old
compressed-image internal formats. It is invoked automatically through the global
`ReadFileCallback` — **but only when `FixedFunctionRemoval` is part of the init flags**:

```cpp
// readerwriter/Utilities.h
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
    DefaultParameters = FixedFunctionRemoval | TangentCreation | GaussianSorting
#else
    DefaultParameters = TangentCreation | GaussianSorting   // desktop GL2: keep it working
#endif
```

Rules of thumb:
- Pass `osgVerse::defaultInitParameters()` (or `osgVerse::DefaultParameters`) to
  `globalInitialize()`; do **not** hand-build the flags with just
  `TangentCreation | GaussianSorting`, or loaded models keep their legacy state and the
  core-profile build floods with `invalid enumerant`.
- If some state is still not handled, extend `FixedFunctionOptimizer` /
  `ModeChecker` in `readerwriter/Utilities.cpp` — do not add a second removal pass.
- State sets created **by the engine at runtime** never go through the load-time
  optimizer, so bind them with `osgVerse::applyTextureAttributeAndModes()`
  (`pipeline/Utilities.h`) instead of `StateSet::setTextureAttributeAndModes()`: in a
  GL3/GLES build it sets the texture attribute without the removed enable (and it never
  sets a mode for a buffer texture).
- The only accepted core-profile error report is the handful of one-time notices during
  the first realized frames (also present in the clean test cases); a repeating report is
  a real bug.

### Known Pitfalls

- **Shader `#include` is resolved by OSG's `osgdb_glsl` plugin, relative to the *including
  shader's directory*.** `ReaderWriterGLSL::readShader` pushes the current file's directory
  onto the options' `databasePathList` and resolves every bare `#include "x.module.glsl"`
  against it. After the `deferred/`/`forward/` reorganization, shaders in those
  subdirectories must write `#include "../x.module.glsl"`. The include pass in
  `ShaderLibrary::processIncludes` is only a fallback (for scripts that bypass the plugin)
  and resolves data-file → including-shader-relative → `SHADER_DIR` → `SHADER_DIR/deferred`
  → `SHADER_DIR/forward`.
- **Symptoms of a failed include are delayed and misleading:** the plugin logs
  `Error reading file x.module.glsl: file not found` at *shader load* time, but the GL
  compiler only complains much later with `undefined variable <function>` at link time.
  Treat any `file not found` during load as fatal for that shader — do not debug the
  "undefined variable" first.
- **After moving/renaming shader files**, three things must be updated together: the C++
  load paths (`PipelineStandard.cpp` ctor `READ_SHADER`s, `ShadowModule.cpp`, plus direct
  loads in `tests/pipeline_test.cpp`, `tests/shadow_test.cpp`,
  `tests/forward_pbr_test.cpp`, `wasm/pbr_demo/wasm_viewer.cpp`), the *runtime* shader
  tree (`verse-build/<mode>/sdk/shaders` — mirror it with
  `robocopy assets/shaders <runtime>/shaders /MIR` so stale flat copies are deleted, plain
  copy leaves both old and new files), and the include paths above.
- **There is no `osgVersePipeline.dll` on Windows** — the pipeline library is linked
  statically into each test/viewer exe, so rebuilding the test target *is* rebuilding the
  pipeline. But running the exe still needs the full PATH
  (`osg-build\compat_x64\sdk\bin` first for OSG/plugins, then
  `Dependencies\x64\msvc18\bin`, then `verse-build\compat_x64\bin`), and
  `OSG_LIBRARY_PATH=verse-build\compat_x64\sdk\bin`.
- **Do not mix cmd and PowerShell in one command line.** Inside `cmd /c "..."` anything
  with `$` is mangled by the outer PowerShell, and `findstr /c:"a" /c:"b" /i` silently
  mis-parses with several qualifiers. For anything non-trivial write a temporary `.ps1`
  script and run it with `powershell -ExecutionPolicy Bypass -File`, or keep one cmd
  `set ... && exe` line and post-filter the log with `Select-String` in a separate call.
- **"Coverage ≈ 0 on surfaces facing open sky is correct, not a bug"** for SSAO and SSGI
  alike: a wall whose hemisphere sees only the sky has no occluders/bouncers, so the
  fallback (IBL) takes over entirely. Verify GI on interior/corner views (e.g. inside
  Sponza), never judge it from an exterior wall; and remember eye adaptation is disabled
  with `--no-bloom`, so A/B brightness changes are real, not exposure-compensated.
- **Runtime stage toggling pattern** (used by the 1-6 mode switcher in
  `many_lights_test`): set the stage camera's node mask to 0 to disable a stage (the cull
  visitor skips the whole slave, so its buffers keep stale content), then rebind every
  consumer of that stage's outputs with `Stage::applyDefaultTexture(...)` (zero texel) or
  `Stage::applyBuffer(...)` when re-enabling. Rebinding only on the consuming side is not
  enough if an exposure/metering stage also reads the buffer. See `LightStatHandler::
  applyGi` for the full pattern (Ssgi chain + tone mapping unit 6 + luminance unit 4).
- **"No lights" cannot be done by masking the light geode**: the light table is fed by the
  per-drawable cull callback (`LightCullCallback` → `LightGlobalManager`), and entries are
  only pruned after several frames of not being culled. A masked geode keeps lighting the
  scene with the stale table. Zero the `LightDrawable` colors instead (instant, see
  `LightStatHandler::setLights`).
- **`HeadUpDisplayCanvas` has two traps** (hit while moving the many_lights_test HUD):
  1. Its `Anchor` enum values do **not** match `3rdparty/layout.h`'s flags: `BOTTOM` maps
     to `LAY_TOP`, `TOP` maps to `LAY_BOTTOM` — the createText anchor of a multi-line text
     block is effectively unpredictable. Pin the `osgText::Text` object directly instead
     (`setAlignment(LEFT_TOP)` + explicit position after `createText`).
  2. The canvas size must come from the **real framebuffer**, not a hardcoded 1920x1080:
     a DPI-scaled window (1280x720 request on a 200% Windows display → 2560x1600
     framebuffer) renders a 1920x1080 canvas 1:1 into the bottom-left of the framebuffer,
     so content can never reach the top third of the screen. In tests, create the canvas
     after realize() using the master camera's viewport (`viewer->getCamera()->
     getViewport()` — its GraphicsContext is often NULL because the stages render via
     slaves; the viewport is the reliable size source).
- **`--agent-keys` presses are delivered as real key events** (keyPress+keyRelease through
  the event queue at the capture frame), but verify the *effect* numerically — a wrong
  toggle assumption cost a debug round here: compare the capture against the expected
  configuration's reference image, not against "it changed something".
- **The forward pass output is NOT tone mapped** in the standard pipeline: the forward
  camera draws to the back buffer after the display stage. Expect modes 1/2 of
  `many_lights_test` (forward) to look blown out versus deferred — that is the pipeline's
  known limitation, not a bug in the test (see plan item 2 above).
- **`osg::TextureBuffer` + `setImage()` needs `dirtyTextureObject()`.** `apply()` calls
  `glTexBuffer()` only when the GL texture object is first created; `setImage()`
  (through `setBufferData()`) swaps the `BufferData`/`BufferObject` without touching the
  texture object. The texture then keeps sampling the *previous* buffer forever. Whenever
  a buffer texture's storage is re-allocated (e.g. `LightCluster::ensureIndexCapacity`),
  call `dirtyTextureObject()` afterwards or the shader reads stale data — which shows up
  as cluster-shaped artifacts that appear once the buffer grows and never go away.
- **Never set a fixed-function texture enable for a TBO** (`GL_TEXTURE_BUFFER` is not a
  valid `glEnable` target); see `applyTextureAttributeAndModes()`.
- **Landing a change in the right place:** after editing a library or a test you must
  rebuild *and* `install` the mode's build tree, otherwise `build/sdk_<mode>` keeps the
  stale binaries and the old behaviour.

### Compatibility Profile on Modern Drivers (Moore Threads M1000)

- A compatibility context can report GL 4.6 / GLSL 4.60, so `Pipeline` keeps its modern
  target version and `ShaderLibrary` emits `#version 460 compatibility`. Some drivers then
  reject MRT writes through `gl_FragData` even though `GL_MAX_DRAW_BUFFERS` is 8: the M1000
  reports `Active array size for gl_FragData is 2, greater than number of draw buffers
  supported: 1` (the same shader compiles at `#version 330 compatibility` *and* in core).
  Do not "fix" this by lowering the version blindly — use explicit fragment outputs.
- MRT shaders write `layout(location = N) VERSE_FS_OUT ...` whenever `VERSE_GLES3`
  (GL3/GLES3 builds) or `VERSE_FS_MRT` (compatibility builds with GLSL >= 330, defined in
  `ShaderLibrary::createShaderDefinitions`) is set, and fall back to `gl_FragData` only on
  older contexts. The same `#if defined(VERSE_GLES3) || defined(VERSE_FS_MRT)` guard is used
  in `standard_pipeline.json`, `standard_pipeline_wasm.json` and the `.glsl` modules.
- Reproduce a compiler verdict without the engine before blaming osgVerse: create a 4.6
  compatibility context (`GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB`) and compile a tiny
  fragment shader writing `gl_FragData[0..3]`. The M1000 compiler also prints
  `Maybe unsupported #version profile: compatibility`, but that line is only a warning.

### SIGSEGV at Exit (`_dl_fini` / Driver Teardown)

- A crash after the last frame whose stack ends in `_dl_fini` -> `mtgpu_dri.so` ->
  `dlclose("libdrm.so.2")` -> `unlink_chunk`/`malloc_consolidate` is **not** a GL bug of the
  pipeline: the `main` arena is already corrupt when the driver's destructor runs. Here the
  origin is OSG's static destruction order: `s_extensions`, `s_glExtensionSetList`,
  `s_glRendererList` and `s_glInitializedList` in `src/osg/GLExtensions.cpp` are
  function-scope statics, while the `ContextData` map (and the `VertexArrayState`s it
  keeps) can release the last `GLExtensions` reference later. `~GLExtensions()` then resets
  buffers that were already destroyed and corrupts the heap. The bloom / eye-adaptation
  chain makes it visible because it registers many more geometries (and their per-context
  `VertexArrayState`s), so the last reference survives until static destruction.
- `Setup.sh` patches those four statics to intentionally leak their single instance, which
  removes the dependency on the destruction order (a few kB leaked once at exit). See the
  "Automatically patching source code" block and the comment in the patched OSG source.
  The patch is applied only when Setup.sh rebuilds OSG: after updating an existing tree,
  answer `n` to "Would you like to use current OSG built" once so the patched libosg is
  compiled and installed.
- Diagnosis that actually works: `MALLOC_CHECK_=3` (or
  `GLIBC_TUNABLES=glibc.malloc.check=3:glibc.malloc.tcache_count=0`) reports
  `corrupted double-linked list`; under `gdb` the same chunk address appears on every run
  with `set disable-randomization on`. Break on `exit` and call `malloc_trim(0)` (after a
  malloc/free loop that drains the tcache bins) to prove the heap is still sane *before*
  the exit handlers: if it is, the corruption happens in destructors, not while rendering.
- `osgviewer` from the same SDK may abort earlier for an unrelated reason
  (`std::string` constructed from a null extension name in
  `osg::isGLExtensionOrVersionSupported`); validate the driver with the osgVerse tests
  (`osgVerse_Test_Pipeline`, `osgVerse_Test_Many_Lights`) instead of plain `osgviewer`.

### Serialization Wrappers (wrappers/)
Custom osgVerse classes are serialized through `osgDB::ObjectWrapper`
serializers. Each wrapper is a cpp file in `wrappers/` that builds an
`osgDB::ObjectWrapper` using macros like `ADD_USER_SERIALIZER`,
`ADD_VECTOR_SERIALIZER`, `REGISTER_METHOD` (see `wrappers/Geometry_FixedWrapper.cpp`
and `wrappers/WrapperMethods.cpp`). These keep `.osg/.osgb` scenes with custom
node types working with stock `osgDB::readNodeFile`. `updateOsgBinaryWrappers()`
fixes binary wrappers after loading; it is called by external tools via
`osgVerse::updateOsgBinaryWrappers(...)`.

### Memory Management
- Uses OSG's reference counting (`osg::ref_ptr`)
- Custom allocators available via `VERSE_USE_MIMALLOC`

### SIMD Support
Automatic SIMD detection and compilation:
- SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2
- AVX/AVX2/AVX512
- Automatic flags applied via `VERSE_SIMD_FEATURES`

## Security Considerations

1. **Buffer Overflow**: Use OSG's array classes and bounds checking
2. **Shader Injection**: Validate all shader inputs
3. **File I/O**: Use OSG's virtual file system for sandboxing
4. **Network**: libhv includes mbedtls for TLS support

## Troubleshooting

### Common Build Issues

**OpenSceneGraph not found:**
- Set `OSG_ROOT` environment variable or CMake variable
- Or place OSG source at `../OpenSceneGraph`

**GLES libraries not found:**
- For GLES builds, provide path to libEGL.so and libGLESv2.so
- Use `Setup.sh <path_to_gles_libs>`

**CUDA/Compiler incompatibility:**
- Check CUDA version against compiler version compatibility
- See CMakeLists.txt line ~353-396 for version checks

**Static build issues:**
- Enable `VERSE_USE_OSG_STATIC` to use static OSG
- This forces `VERSE_STATIC_BUILD` automatically

### Platform-Specific Notes

**Linux (Kylin/UOS):**
- May need `VERSE_FIND_LEGACY_OPENGL=ON`
- May need `VERSE_USE_GLIBCXX11_ABI=OFF` for older systems

**macOS:**
- Uses `.so` suffix for shared libraries
- May need Google Angle for GLES support

**Windows:**
- Use Visual Studio 2017-2022 recommended
- MSYS2/MinGW also supported
- PDB files installed with `VERSE_INSTALL_PDB_FILES=ON`

## Resources

- **README.md**: Full project documentation
- **CODE_STYLE.md**: Detailed coding standards
- **TODO_cn.md**: Development roadmap (Chinese)
- **THIRDPARTY_LICENSES.md**: Third-party license information

## Contributing

When contributing:
1. Follow the code style in `CODE_STYLE.md`
2. Test on multiple platforms if possible
3. Update this file if build processes change
4. Add tests for new features
