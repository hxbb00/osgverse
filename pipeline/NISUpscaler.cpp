// NISUpscaler.cpp - Implementation of NVIDIA Image Scaling for osgVerse
//
// This file implements NISUpscaler which wraps NVIDIA Image Scaling SDK
// compute shaders for use in osgVerse pipeline stages.
//
// Design principles:
//   - NIS_Config.h and NIS_Scaler.h are NOT modified (used as-is from SDK)
//   - All GL 4.5 compute dispatch is done through osg::GLExtensions
//   - Coefficient textures are generated from constexpr arrays in NIS_Config.h
//   - Uniform buffer matches std140 layout of GLSL uniform block

#include "NISUpscaler.h"
#include "Global.h"
#include "Utilities.h"
#include <osg/Version>
#include <osg/FrameBufferObject>
#include <osg/GLExtensions>
#include <osg/Geode>
#include <osg/Shader>
#include <osg/DeleteHandler>
#include <osgViewer/View>
#include <osgDB/FileUtils>
#include <fstream>
#include <sstream>
#include <cstring>
#include "NIS_Config.h"

// GL defines for compatibility
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif

#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif

// NIS relies on GL 4.3 compute shaders: desktop OpenGL contexts (core or compatibility profile)
// provide them, while the GLES/WebGL builds of osgVerse do not. The actual GL version is checked
// at runtime as well, so a desktop build without a 4.3+ context reports an error and does nothing
// instead of silently producing a broken upscaling chain
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
#  define VERSE_NIS_COMPUTE_SUPPORTED 0
#else
#  define VERSE_NIS_COMPUTE_SUPPORTED 1
#endif

namespace osgVerse
{
    // ========================================================================
    // Embedded GLSL compute shader source
    // The SDK header is inlined by createShaderProgram() (GLSL has no portable #include),
    // so all uniforms and samplers it uses must be declared before the include line
    // ========================================================================
    static const char* s_nisComputeGlsl = R"GLSL(
#version 430
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#define NIS_GLSL 1
#define NIS_SCALER %NIS_SCALER%
#define NIS_HDR_MODE %NIS_HDR_MODE%
#define NIS_BLOCK_WIDTH %NIS_BLOCK_WIDTH%
#define NIS_BLOCK_HEIGHT %NIS_BLOCK_HEIGHT%
#define NIS_THREAD_GROUP_SIZE %NIS_THREAD_GROUP_SIZE%
#define NIS_VIEWPORT_SUPPORT 0
#define NIS_CLAMP_OUTPUT 1

// Uniform block - std140 layout matches NISConfig struct from NIS_Config.h
layout(std140, binding=0) uniform NISConfigBlock
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;
    float kContrastBoost;
    float kEps;
    float kSharpStartY;
    float kSharpScaleY;
    float kSharpStrengthMin;
    float kSharpStrengthScale;
    float kSharpLimitMin;
    float kSharpLimitScale;
    float kScaleX;
    float kScaleY;
    float kDstNormX;
    float kDstNormY;
    float kSrcNormX;
    float kSrcNormY;
    uint  kInputViewportOriginX;
    uint  kInputViewportOriginY;
    uint  kInputViewportWidth;
    uint  kInputViewportHeight;
    uint  kOutputViewportOriginX;
    uint  kOutputViewportOriginY;
    uint  kOutputViewportWidth;
    uint  kOutputViewportHeight;
    float reserved0;
    float reserved1;
};

layout(binding=1) uniform sampler2D in_texture;
layout(binding=2, rgba8) uniform writeonly image2D out_texture;

#if NIS_SCALER
layout(binding=3) uniform sampler2D coef_scaler;
layout(binding=4) uniform sampler2D coef_usm;
#endif

layout(local_size_x=%NIS_THREAD_GROUP_SIZE%) in;

// Inlined SDK header (its Vulkan-style texture macros are rewritten to OpenGL ones)
#include "NIS_Scaler.h"

void main()
{
#if NIS_SCALER
    NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
#else
    NVSharpen(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
#endif
}
)GLSL";

    // ========================================================================
    NISUpscaler::NISUpscaler(Mode mode, GPUArchitecture gpuArch)
    :   _mode(mode), _inputWidth(0), _inputHeight(0), _outputWidth(0), _outputHeight(0),
        _sharpness(0.5f), _hdrMode(HDR_NONE), _uboId(0), _initialized(false)
    {
        _optimizer = new NISOptimizer(mode == SCALER, (NISGPUArchitecture)gpuArch);
        _blockWidth = _optimizer->GetOptimalBlockWidth();
        _blockHeight = _optimizer->GetOptimalBlockHeight();
        _threadGroupSize = _optimizer->GetOptimalThreadGroupSize();

        _config = new NISConfig;
        OSG_INFO << "[NISUpscaler] Created " << getModeName()
                 << " (block=" << _blockWidth << "x" << _blockHeight
                 << ", threads=" << _threadGroupSize << ")" << std::endl;
    }

    NISUpscaler::~NISUpscaler()
    {
        destroyGLObjects();
        delete _config; delete _optimizer;
    }

    void NISUpscaler::destroyGLObjects()
    {
        if (_uboId != 0)
        {
            // FIXME: actual GL deletion should happen in GL context
            ////osg::ref_ptr<osg::DeleteHandler> dh;
            _uboId = 0;
        }

        _program = NULL;
        _coefScalerTexture = NULL;
        _coefUSMTexture = NULL;
        _initialized = false;
    }

    bool NISUpscaler::initialize(int inputWidth, int inputHeight, int outputWidth, int outputHeight,
                                 float sharpness, HDRMode hdr, const std::string& computeGLSL)
    {
#if !VERSE_NIS_COMPUTE_SUPPORTED
        OSG_WARN << "[NISUpscaler] NIS needs GL 4.3+ compute shaders, which are not available in "
                 << "GLES/WebGL builds; " << inputWidth << "x" << inputHeight << " -> "
                 << outputWidth << "x" << outputHeight << " is not initialized" << std::endl;
        return false;
#else
        if (inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 || outputHeight <= 0)
        {
            OSG_WARN << "[NISUpscaler] Invalid dimensions: input=" << inputWidth << "x" << inputHeight
                     << ", output=" << outputWidth << "x" << outputHeight << std::endl;
            return false;
        }
        _inputWidth = inputWidth; _inputHeight = inputHeight;
        _outputWidth = outputWidth; _outputHeight = outputHeight;
        _sharpness = sharpness; _hdrMode = hdr;

        // Validate scaler constraints: 0.5 <= scale <= 1.0
        if (_mode == SCALER)
        {
            float sx = float(inputWidth) / float(outputWidth);
            float sy = float(inputHeight) / float(outputHeight);
            if (sx < 0.5f || sx > 1.0f || sy < 0.5f || sy > 1.0f)
            {
                OSG_WARN << "[NISUpscaler] SCALER requires 0.5 <= scale <= 1.0, got "
                         << sx << "x" << sy << std::endl;
                return false;
            }
        }

        destroyGLObjects();
        if (!createCoefficientTextures()) return false;  // Create coefficient textures (scaler needs both, sharpen none)
        if (!createShaderProgram(computeGLSL)) return false;  // Compile compute shader program

        // Generate UBO handle (upload deferred until first dispatch with valid context)
        _uboId = 99999; // marker; actual GL object created on first dispatch

        // Update NISConfig and prepare UBO data
        if (!updateConfig(sharpness, hdr))
        { OSG_WARN << "[NISUpscaler] Config update failed" << std::endl; return false; }

        OSG_NOTICE << "[NISUpscaler] Initialized " << getModeName()
                   << ": " << inputWidth << "x" << inputHeight
                   << " -> " << outputWidth << "x" << outputHeight << std::endl;
        _initialized = true; return true;
#endif
    }

    bool NISUpscaler::updateConfig(float sharpness, HDRMode hdr)
    {
        NISHDRMode nisHdr = NISHDRMode::None;
        switch (hdr)
        {
            case HDR_LINEAR: nisHdr = NISHDRMode::Linear; break;
            case HDR_PQ:     nisHdr = NISHDRMode::PQ; break;
            default:         nisHdr = NISHDRMode::None; break;
        }

        bool ok = false; _sharpness = sharpness; _hdrMode = hdr;
        if (_mode == SCALER)
        {
            ok = NVScalerUpdateConfig(*_config, sharpness,
                0, 0, _inputWidth, _inputHeight, _inputWidth, _inputHeight,
                0, 0, _outputWidth, _outputHeight, _outputWidth, _outputHeight, nisHdr);
        }
        else
        {
            ok = NVSharpenUpdateConfig(*_config, sharpness,
                0, 0, _inputWidth, _inputHeight, _inputWidth, _inputHeight, 0, 0, nisHdr);
        }
        if (ok) updateUniformBuffer();
        return ok;
    }

    bool NISUpscaler::createCoefficientTextures()
    {
        // NIS uses 64 phases x 8 taps (scaler uses first 6, sharpen uses all 8)
        const int texW = 2;   // 8 floats / 4 (rgba) = 2 texels per phase row
        const int texH = 64;  // kPhaseCount

        // Scaler coefficients (coef_scale from NIS_Config.h anonymous namespace)
        {
            osg::ref_ptr<osg::Image> img = new osg::Image;
            img->allocateImage(texW, texH, 1, GL_RGBA, GL_FLOAT);
            std::memset(img->data(), 0, img->getTotalSizeInBytes());

            float* dst = (float*)img->data();
            for (int p = 0; p < 64; ++p)
                for (int t = 0; t < 8; ++t)
                    dst[p * texW * 4 + t] = coef_scale[p][t];

            _coefScalerTexture = new osg::Texture2D(img.get());
            _coefScalerTexture->setInternalFormat(GL_RGBA32F);
            _coefScalerTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            _coefScalerTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            _coefScalerTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            _coefScalerTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        }

        // USM coefficients (coef_usm from NIS_Config.h anonymous namespace)
        {
            osg::ref_ptr<osg::Image> img = new osg::Image;
            img->allocateImage(texW, texH, 1, GL_RGBA, GL_FLOAT);
            std::memset(img->data(), 0, img->getTotalSizeInBytes());

            float* dst = (float*)img->data();
            for (int p = 0; p < 64; ++p)
                for (int t = 0; t < 8; ++t)
                    dst[p * texW * 4 + t] = coef_usm[p][t];

            _coefUSMTexture = new osg::Texture2D(img.get());
            _coefUSMTexture->setInternalFormat(GL_RGBA32F);
            _coefUSMTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            _coefUSMTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            _coefUSMTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            _coefUSMTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        }
        return true;
    }

    bool NISUpscaler::createShaderProgram(const std::string& computeGLSL)
    {
        // Build shader source by substituting template parameters
        std::string src = computeGLSL.empty() ? s_nisComputeGlsl: computeGLSL;
        std::string scalerVal = (_mode == SCALER) ? "1" : "0";
        std::string hdrVal = (_hdrMode == HDR_LINEAR) ? "1" : ((_hdrMode == HDR_PQ) ? "2" : "0");
        std::string bw = std::to_string(_blockWidth);
        std::string bh = std::to_string(_blockHeight);
        std::string tgs = std::to_string(_threadGroupSize);

        auto replace = [](std::string& text, const std::string& key, const std::string& val)
        {
            size_t pos = 0;
            while ((pos = text.find(key)) != std::string::npos)
                text.replace(pos, key.length(), val);
        };
        replace(src, "%NIS_SCALER%", scalerVal);
        replace(src, "%NIS_HDR_MODE%", hdrVal);
        replace(src, "%NIS_BLOCK_WIDTH%", bw);
        replace(src, "%NIS_BLOCK_HEIGHT%", bh);
        replace(src, "%NIS_THREAD_GROUP_SIZE%", tgs);

        // GLSL has no portable '#include': inline the SDK header from the shader directory.
        // The header defines Vulkan-style combined image samplers, so its texture macros are
        // rewritten to the equivalent OpenGL sampler/image functions
        const std::string includeTag = "#include \"NIS_Scaler.h\"";
        size_t includePos = src.find(includeTag);
        if (includePos != std::string::npos)
        {
            std::string headerFile = SHADER_DIR + "NIS_Scaler.h";
            std::ifstream fin(headerFile.c_str(), std::ios::in | std::ios::binary);
            std::string header((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
            if (header.empty())
            {
                OSG_WARN << "[NISUpscaler] Can not read " << headerFile
                         << ", please check SHADER_DIR" << std::endl;
                return false;
            }
            replace(header, "sampler2D(x, samplerLinearClamp)", "x");
            replace(header, "sampler2D(x, sampler)", "x");
            src.replace(includePos, includeTag.length(), header);
        }

#if OSG_VERSION_GREATER_THAN(3, 1, 5)
        osg::ref_ptr<osg::Shader> cs = new osg::Shader(osg::Shader::COMPUTE, src);
        cs->setName(std::string("NIS_") + getModeName());
        _program = new osg::Program; _program->setName(getModeName());
        _program->addBindUniformBlock("NISConfigBlock", 0);  // matches glBindBufferBase()
        _program->addShader(cs.get()); return true;
#else
        OSG_WARN << "[NISUpscaler] Compute shader not available" << std::endl;
        return false;
#endif
    }

    void NISUpscaler::updateUniformBuffer()
    {
        // UBO data is kept in _config; uploaded to GPU in dispatch()
        // No action needed here since _config is already updated
    }

    void NISUpscaler::dispatch(osg::State* state)
    {
        unsigned int ctxID = state->getContextID();
        if (!_initialized || !_program.valid()) return;
#if OSG_VERSION_GREATER_THAN(3, 5, 0)
        osg::GLExtensions* ext = state->get<osg::GLExtensions>();
        if (!ext || !ext->glDispatchCompute)
        {
            OSG_WARN << "[NISUpscaler] glDispatchCompute not available (needs GL 4.3+)" << std::endl;
            return;
        }

        // Compile and bind shader program
        _program->compileGLObjects(*state);
        _program->apply(*state);

        // Bind the samplers explicitly as well: not every driver honours layout(binding=...)
        osg::Program::PerContextProgram* pcp = _program->getPCP(*state);
        GLuint programId = pcp ? pcp->getHandle() : 0;
        if (programId != 0)
        {
            ext->glUniform1i(ext->glGetUniformLocation(programId, "in_texture"), 1);
            ext->glUniform1i(ext->glGetUniformLocation(programId, "out_texture"), 2);
            ext->glUniform1i(ext->glGetUniformLocation(programId, "coef_scaler"), 3);
            ext->glUniform1i(ext->glGetUniformLocation(programId, "coef_usm"), 4);
        }

        // Ensure textures have GL objects
        if (_inputTexture.valid()) _inputTexture->apply(*state);
        if (_outputTexture.valid()) _outputTexture->apply(*state);
        if (_coefScalerTexture.valid()) _coefScalerTexture->apply(*state);
        if (_coefUSMTexture.valid()) _coefUSMTexture->apply(*state);

        // --- Create UBO on first dispatch (need active GL context) ---
        if (_uboId == 99999)
        {
            ext->glGenBuffers(1, &_uboId);
            if (_uboId == 0) { OSG_WARN << "[NISUpscaler] Failed to generate UBO" << std::endl; return; }
            ext->glBindBuffer(GL_UNIFORM_BUFFER, _uboId);
            ext->glBufferData(GL_UNIFORM_BUFFER, sizeof(NISConfig), _config, GL_DYNAMIC_DRAW);
        }
        else
        {   // Update existing UBO
            ext->glBindBuffer(GL_UNIFORM_BUFFER, _uboId);
            ext->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(NISConfig), _config);
        }
        ext->glBindBufferBase(GL_UNIFORM_BUFFER, 0, _uboId);

        // --- Bind textures to units ---
        if (_inputTexture.valid() && _inputTexture->getTextureObject(ctxID))
        {   // Unit 1: input texture
            ext->glActiveTexture(GL_TEXTURE0 + 1);
            _inputTexture->apply(*state);
        }

        // Unit 2: output as image (for imageStore)
        if (_outputTexture.valid() && _outputTexture->getTextureObject(ctxID))
        {
            GLuint texId = _outputTexture->getTextureObject(ctxID)->id();
            ext->glBindImageTexture(2, texId, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        }

        // Units 3,4: coefficient textures (scaler only)
        if (_mode == SCALER)
        {
            if (_coefScalerTexture.valid() && _coefScalerTexture->getTextureObject(ctxID))
            {
                ext->glActiveTexture(GL_TEXTURE0 + 3);
                _coefScalerTexture->apply(*state);
            }
            if (_coefUSMTexture.valid() && _coefUSMTexture->getTextureObject(ctxID))
            {
                ext->glActiveTexture(GL_TEXTURE0 + 4);
                _coefUSMTexture->apply(*state);
            }
        }
        ext->glActiveTexture(GL_TEXTURE0);

        // --- Dispatch compute ---
        unsigned int groupsX = (_outputWidth  + _blockWidth  - 1) / _blockWidth;
        unsigned int groupsY = (_outputHeight + _blockHeight - 1) / _blockHeight;
        ext->glDispatchCompute(groupsX, groupsY, 1);

        // Ensure image writes complete before the result is sampled by the following pass
        ext->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        // FIXME: Restore default program (let OSG manage state)?
        // state->apply(static_cast<osg::Program*>(NULL));
#else
        OSG_WARN << "[NISUpscaler] glDispatchCompute not available (needs OSG 3.5+)" << std::endl;
#endif
    }

    void NISUpscaler::applyShader(osg::StateSet* ss) const
    { if (_program.valid()) ss->setAttribute(_program.get()); }

    // ========================================================================
    // Helpers for building a standalone NIS rendering chain
    // ========================================================================
    void NISMirrorCameraCallback::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        osg::Camera* camera = static_cast<osg::Camera*>(node);
        if (_mainCamera.valid())
        {
            camera->setViewMatrix(_mainCamera->getViewMatrix());
            camera->setProjectionMatrix(_mainCamera->getProjectionMatrix());
        }
        traverse(node, nv);
    }

    NISLayoutCallback::NISLayoutCallback(osg::Camera* sceneCamera, osg::Camera* displayCamera,
                                         osg::Geometry* quad, int inputWidth, int inputHeight,
                                         int outputWidth, int outputHeight)
    :   _sceneCamera(sceneCamera), _displayCamera(displayCamera), _quad(quad),
        _inputWidth(inputWidth), _inputHeight(inputHeight),
        _outputWidth(outputWidth), _outputHeight(outputHeight), _lastWidth(0), _lastHeight(0)
    {}

    void NISLayoutCallback::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        const osg::GraphicsContext* gc = _displayCamera->getGraphicsContext();
        const osg::GraphicsContext::Traits* traits = gc ? gc->getTraits() : NULL;
        int w = traits ? traits->width : 0, h = traits ? traits->height : 0;
        if (w > 0 && h > 0 && (w != _lastWidth || h != _lastHeight))
        {
            _lastWidth = w; _lastHeight = h;
            _sceneCamera->setViewport(new osg::Viewport(0, 0, _inputWidth, _inputHeight));
            _displayCamera->setViewport(new osg::Viewport(0, 0, w, h));
            _displayCamera->setProjectionMatrix(osg::Matrix::ortho(0.0, w, 0.0, h, -1.0, 1.0));
            NISUpscaler::fitQuadToWindow(_quad.get(), _outputWidth, _outputHeight, w, h);
        }
        traverse(node, nv);
    }

    osg::Texture2D* NISUpscaler::createTexture(int width, int height)
    {
        if (width < 1 || height < 1) return NULL;
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setInternalTextureFormat(GL_RGBA8);
        std::memset(image->data(), 0, image->getTotalSizeInBytes());

        osg::Texture2D* texture = new osg::Texture2D(image.get());
        texture->setTextureSize(width, height);
        texture->setInternalFormat(GL_RGBA8);
        texture->setResizeNonPowerOfTwoHint(false);  // keep the exact FBO / upscaled size
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return texture;
    }

    osg::Geometry* NISUpscaler::createOutputQuad()
    {
        osg::Geometry* quad = osg::createTexturedQuadGeometry(
            osg::Vec3(0.0f, 0.0f, 0.0f), osg::Vec3(1.0f, 0.0f, 0.0f), osg::Vec3(0.0f, 1.0f, 0.0f));
        quad->setName("osgVerse::NISOutputQuad");
        osg::Vec4Array* colors = new osg::Vec4Array;
        colors->push_back(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
        quad->setColorArray(colors, osg::Array::BIND_OVERALL);

        osg::StateSet* ss = quad->getOrCreateStateSet();
        ss->setAttribute(createDefaultProgram("NISOutput"));
        ss->addUniform(new osg::Uniform("NISOutput", (int)0));
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        ss->setMode(GL_BLEND, osg::StateAttribute::OFF);
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif
        return quad;
    }

    void NISUpscaler::fitQuadToWindow(osg::Geometry* quad, int outW, int outH, int winW, int winH)
    {
        if (quad == NULL || outW < 1 || outH < 1 || winW < 1 || winH < 1) return;
        osg::Vec3Array* va = static_cast<osg::Vec3Array*>(quad->getVertexArray());
        if (va == NULL || va->size() < 4) return;

        // Keep the output aspect ratio and center it in the window (letterbox / pillarbox)
        float outAspect = (float)outW / (float)outH, winAspect = (float)winW / (float)winH;
        float sx = (float)winW, sy = (float)winH;
        if (outAspect > winAspect) sy = (float)winW / outAspect;
        else sx = (float)winH * outAspect;

        // Vertex order must match osg::createTexturedQuadGeometry():
        // 0 = (0, h), 1 = (0, 0), 2 = (w, 0), 3 = (w, h)
        float x0 = ((float)winW - sx) * 0.5f, y0 = ((float)winH - sy) * 0.5f;
        (*va)[0].set(x0, y0 + sy, 0.0f); (*va)[1].set(x0, y0, 0.0f);
        (*va)[2].set(x0 + sx, y0, 0.0f); (*va)[3].set(x0 + sx, y0 + sy, 0.0f);
        va->dirty(); quad->dirtyBound();
    }

    NISUpscalingView NISUpscaler::createUpscalingView(
        osgViewer::View* view, osg::Node* scene, int inputWidth, int inputHeight,
        int outputWidth, int outputHeight, float sharpness, HDRMode hdr, Mode mode)
    {
        NISUpscalingView chain;
#if !VERSE_NIS_COMPUTE_SUPPORTED
        OSG_WARN << "[NISUpscaler] NIS upscaling needs GL 4.3+ compute shaders, which are not "
                 << "available in GLES/WebGL builds; no upscaling chain is created and the view is "
                 << "left unchanged" << std::endl;
        return chain;
#else
        if (view == NULL || view->getCamera() == NULL)
        {
            OSG_WARN << "[NISUpscaler] No view to build the upscaling chain" << std::endl;
            return chain;
        }

        osg::Camera* mainCamera = view->getCamera();
        osg::GraphicsContext* gc = mainCamera->getGraphicsContext();
        if (gc == NULL || !gc->isRealized())
        {
            OSG_WARN << "[NISUpscaler] The view has to be realized before creating the chain"
                     << std::endl;
            return chain;
        }
        if (inputWidth < 1 || inputHeight < 1 || outputWidth < 1 || outputHeight < 1)
        {
            OSG_WARN << "[NISUpscaler] Invalid upscaling sizes: " << inputWidth << "x"
                     << inputHeight << " -> " << outputWidth << "x" << outputHeight << std::endl;
            return chain;
        }

        gc->makeCurrent();
        float glVersion = osg::getGLVersionNumber();
        gc->releaseContext();
        if (glVersion < 4.3f)
        {
            OSG_WARN << "[NISUpscaler] NIS needs OpenGL 4.3+ for compute shaders, got "
                     << glVersion << std::endl;
            return chain;
        }

        // Textures and compute program first: if anything fails the view is left untouched
        chain.inputTexture = createTexture(inputWidth, inputHeight);
        chain.outputTexture = createTexture(outputWidth, outputHeight);
        chain.upscaler = new NISUpscaler(mode);
        if (!chain.upscaler->initialize(inputWidth, inputHeight, outputWidth, outputHeight,
                                        sharpness, hdr))
        {
            OSG_WARN << "[NISUpscaler] Failed to initialize " << chain.upscaler->getModeName()
                     << ", no upscaling chain is created" << std::endl;
            chain = NISUpscalingView(); return chain;
        }
        chain.upscaler->setInputTexture(chain.inputTexture.get());
        chain.upscaler->setOutputTexture(chain.outputTexture.get());

        // Offscreen camera rendering the scene into the small FBO
        osg::Camera* sceneCamera = new osg::Camera;
        sceneCamera->setName("osgVerse::NISScene");
        sceneCamera->setRenderOrder(osg::Camera::PRE_RENDER, 0);
        sceneCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        sceneCamera->setAllowEventFocus(false);
        sceneCamera->setGraphicsContext(gc);
        sceneCamera->setViewport(new osg::Viewport(0, 0, inputWidth, inputHeight));
        sceneCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        sceneCamera->attach(osg::Camera::COLOR_BUFFER0, chain.inputTexture.get());
        sceneCamera->setUpdateCallback(new NISMirrorCameraCallback(mainCamera));

        // Presentation camera: the NIS result is computed in its pre-draw callback
        const osg::GraphicsContext::Traits* traits = gc->getTraits();
        int winW = (traits && traits->width > 0) ? traits->width : outputWidth;
        int winH = (traits && traits->height > 0) ? traits->height : outputHeight;
        chain.displayQuad = createOutputQuad();
        chain.displayQuad->getOrCreateStateSet()->setTextureAttributeAndModes(
            0, chain.outputTexture.get(), osg::StateAttribute::ON);

        osg::Camera* displayCamera = createHUDCamera(NULL, winW, winH);
        displayCamera->setName("osgVerse::NISDisplay");
        displayCamera->setRenderOrder(osg::Camera::POST_RENDER, 1);
        displayCamera->setGraphicsContext(gc);
        displayCamera->setProjectionResizePolicy(osg::Camera::FIXED);
        displayCamera->setProjectionMatrix(osg::Matrix::ortho(0.0, winW, 0.0, winH, -1.0, 1.0));
        displayCamera->setPreDrawCallback(new NISDrawCallback(chain.upscaler.get()));
        displayCamera->setUpdateCallback(new NISLayoutCallback(
            sceneCamera, displayCamera, chain.displayQuad.get(), inputWidth, inputHeight,
            outputWidth, outputHeight));
        osg::Geode* geode = new osg::Geode; geode->addDrawable(chain.displayQuad.get());
        displayCamera->addChild(geode);
        fitQuadToWindow(chain.displayQuad.get(), outputWidth, outputHeight, winW, winH);

        // The scene is rendered at the internal resolution: keep the interactive projection
        // aspect as well, and letterbox the presented result when the window changes size
        double fovy = 30.0, aspect = 1.0, znear = 1.0, zfar = 10000.0;
        if (mainCamera->getProjectionMatrix().getPerspective(fovy, aspect, znear, zfar))
        {
            mainCamera->setProjectionMatrixAsPerspective(
                fovy, (double)inputWidth / (double)inputHeight, znear, zfar);
            mainCamera->setProjectionResizePolicy(osg::Camera::FIXED);
        }

        // Move the scene to the offscreen camera and register both slaves: the interactive main
        // camera then only keeps the manipulator, so the scene is not rendered twice
        if (scene != NULL)
        {
            if (mainCamera->containsNode(scene)) mainCamera->removeChild(scene);
            sceneCamera->addChild(scene);
        }
        view->addSlave(sceneCamera, false);
        view->addSlave(displayCamera, false);

        chain.sceneCamera = sceneCamera; chain.displayCamera = displayCamera;
        OSG_NOTICE << "[NISUpscaler] Upscaling chain: " << inputWidth << "x" << inputHeight
                   << " -> " << outputWidth << "x" << outputHeight << " (window " << winW << "x"
                   << winH << ", " << chain.upscaler->getModeName() << ", sharpness "
                   << sharpness << ")" << std::endl;
        return chain;
#endif
    }
}
