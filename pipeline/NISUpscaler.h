#ifndef MANA_PP_NISUPSCALER_HPP
#define MANA_PP_NISUPSCALER_HPP

#include <osg/Referenced>
#include <osg/Texture2D>
#include <osg/Shader>
#include <osg/Program>
#include <osg/Camera>
#include <osg/Geometry>
#include <osg/NodeCallback>
#include <osg/RenderInfo>
#include <osg/StateSet>

struct NISOptimizer;
struct NISConfig;

namespace osgViewer { class View; }

namespace osgVerse
{
    class NISUpscaler;
    struct NISUpscalingView;

    /**
     * Callback mirroring the view/projection of the interactive (main) camera into the offscreen
     * camera of a NIS upscaling chain. The interactive camera keeps the manipulator, so the user
     * interaction is not affected by the upscaling.
     */
    class NISMirrorCameraCallback : public osg::NodeCallback
    {
    public:
        NISMirrorCameraCallback(osg::Camera* mainCamera) : _mainCamera(mainCamera) {}
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv);

    protected:
        osg::observer_ptr<osg::Camera> _mainCamera;
    };

    /**
     * Callback keeping the presentation camera (viewport, projection and screen quad) in sync
     * with the window size, while the offscreen render camera keeps its fixed resolution.
     */
    class NISLayoutCallback : public osg::NodeCallback
    {
    public:
        NISLayoutCallback(osg::Camera* sceneCamera, osg::Camera* displayCamera, osg::Geometry* quad,
                          int inputWidth, int inputHeight, int outputWidth, int outputHeight);
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv);

    protected:
        osg::observer_ptr<osg::Camera> _sceneCamera, _displayCamera;
        osg::observer_ptr<osg::Geometry> _quad;
        int _inputWidth, _inputHeight, _outputWidth, _outputHeight;
        int _lastWidth, _lastHeight;
    };

    /**
     * NVIDIA Image Scaling processor for osgVerse.
     *
     * Encapsulates NIS compute shader resources: coefficient textures,
     * uniform buffer object (UBO), and shader program.
     *
     * Typical integration in osgVerse Pipeline:
     *   1. Create NISUpscaler with desired mode (SCALER or SHARPEN)
     *   2. Call initialize() with input/output dimensions
     *   3. Wire input texture from previous pipeline stage (post-tone-mapping)
     *   4. Wire output texture to display stage
     *   5. Use NISDrawCallback on a stage camera to auto-dispatch compute
     *
     * For a standalone (non-pipeline) viewer, createUpscalingView() builds the whole chain at
     * once: offscreen camera -> NIS compute -> screen-space quad on a presentation camera.
     */
    class NISUpscaler : public osg::Referenced
    {
    public:
        enum Mode
        {
            SCALER = 0,   /**< Upscale + Sharpen (input < output, 0.5 <= scale <= 1.0) */
            SHARPEN = 1   /**< Sharpen only (input == output, no scaling) */
        };

        enum HDRMode
        {
            HDR_NONE = 0,
            HDR_LINEAR = 1,
            HDR_PQ = 2
        };

        enum GPUArchitecture
        {
            NVIDIA_Generic = 0,
            AMD_Generic = 1,
            Intel_Generic = 2,
            NVIDIA_Generic_fp16 = 3
        };

        /**
         * Create NIS processor.
         * @param mode    SCALER for upscale+sharpen, SHARPEN for sharpen-only
         * @param gpuArch Target GPU architecture for thread optimization
         */
        NISUpscaler(Mode mode = SCALER, GPUArchitecture gpuArch = GPUArchitecture::NVIDIA_Generic);

        /**
         * Initialize OpenGL resources.
         *
         * Must be called before first use. Creates textures, coefficient data,
         * compiles compute shader, and uploads initial config to GPU.
         *
         * @param inputWidth    Source render resolution width (e.g. 1920)
         * @param inputHeight   Source render resolution height (e.g. 1080)
         * @param outputWidth   Target display resolution width (e.g. 2560)
         * @param outputHeight  Target display resolution height (e.g. 1440)
         * @param sharpness     Sharpness [0.0, 1.0], default 0.5
         * @param hdr           HDR mode for tone-mapped HDR input
         * @param computeGLSL   Compute shader code, empty to use default code
         * @return true if all GL resources created successfully
         */
        bool initialize(int inputWidth, int inputHeight,
                        int outputWidth, int outputHeight,
                        float sharpness = 0.5f, HDRMode hdr = HDR_NONE,
                        const std::string& computeGLSL = std::string());

        /**
         * Reconfigure sharpness/HDR. Cheaper than full reinitialize.
         * Call when sharpness slider changes at runtime.
         * @return true if config valid and UBO updated
         */
        bool updateConfig(float sharpness, HDRMode hdr = HDR_NONE);

        /**
         * Dispatch compute shader. Binds textures/UBO and calls glDispatchCompute.
         *
         * Call this from a draw callback (see NISDrawCallback) or manually
         * at the correct pipeline point (after tone-mapping, before display).
         *
         * @param state Current OSG GL state (provides context ID and extensions)
         */
        void dispatch(osg::State* state);

        /**
         * Convenience: set input texture from previous pipeline stage's output.
         * The texture should contain tone-mapped, gamma-corrected color.
         */
        void setInputTexture(osg::Texture2D* tex) { _inputTexture = tex; }

        /**
         * Convenience: set output texture (will be written by compute shader).
         * This texture should be bound to the next stage (display) as input.
         */
        void setOutputTexture(osg::Texture2D* tex) { _outputTexture = tex; }

        // Direct accessors for pipeline wiring --------------------------------
        osg::Texture2D* getInputTexture() const  { return _inputTexture.get(); }
        osg::Texture2D* getOutputTexture() const { return _outputTexture.get(); }
        osg::Program*   getProgram() const       { return _program.get(); }

        Mode     getMode() const  { return _mode; }
        int      getInputWidth() const   { return _inputWidth; }
        int      getInputHeight() const  { return _inputHeight; }
        int      getOutputWidth() const  { return _outputWidth; }
        int      getOutputHeight() const { return _outputHeight; }
        uint32_t getBlockWidth() const   { return _blockWidth; }
        uint32_t getBlockHeight() const  { return _blockHeight; }
        uint32_t getThreadGroupSize() const { return _threadGroupSize; }
        bool     isInitialized() const   { return _initialized; }

        const char* getModeName() const
        { return (_mode == SCALER) ? "NVScaler" : "NVSharpen"; }

        /**
         * Apply shader to StateSet (for pipeline stage setup).
         * Note: actual GL bindings happen in dispatch().
         */
        void applyShader(osg::StateSet* ss) const;

        // ---- Helpers for quickly building a standalone NIS rendering chain ----

        /**
         * Create an RGBA8 texture of the exact given size (no power-of-two rescaling), which can
         * be used as NIS input (render target) or output (compute image + presentation source).
         */
        static osg::Texture2D* createTexture(int width, int height);

        /** Create the screen-space quad presenting the NIS output (unlit textured program). */
        static osg::Geometry* createOutputQuad();

        /** Fit the quad into the window, keeping the output aspect ratio (letter/pillarbox). */
        static void fitQuadToWindow(osg::Geometry* quad, int outW, int outH, int winW, int winH);

        /**
         * Build a complete NIS upscaling chain on a realized view:
         *   - an offscreen camera renders 'scene' into an inputWidth x inputHeight FBO,
         *   - the NIS compute shader upscales/sharpens it into outputWidth x outputHeight,
         *   - a post-render camera presents the result on the whole window with a screen quad.
         * The view's main camera is left to the manipulator (the given scene node is moved from
         * it to the offscreen camera, or not touched when 'scene' is NULL), so the interactive
         * behaviour does not change. Other nodes that should be presented must be added to the
         * display camera of the returned chain.
         *
         * The window context must support GL 4.3+ compute shaders: request it with
         * osg::DisplaySettings::instance()->setGLContextVersion("4.3") before the window is
         * created. Returns an invalid chain (see NISUpscalingView::valid()) on failure, in which
         * case the view has not been modified.
         *
         * Note: compute shaders need a desktop OpenGL 4.3+ context, so the GLES/WebGL builds of
         * osgVerse only emit an OSG_WARN and fail (callers should then render directly). A GL2
         * build can still use it by creating a 4.3+ compatibility-profile context: request
         * setGLContextVersion("4.3") and a non-zero setGLContextProfileMask(2) on DisplaySettings.
         */
        static NISUpscalingView createUpscalingView(osgViewer::View* view, osg::Node* scene,
            int inputWidth, int inputHeight, int outputWidth, int outputHeight,
            float sharpness = 0.5f, HDRMode hdr = HDR_NONE, Mode mode = SCALER);

    protected:
        virtual ~NISUpscaler();

        bool createTextures(int w, int h);
        bool createOutputTexture(int w, int h);
        bool createCoefficientTextures();
        bool createShaderProgram(const std::string& computeGLSL);
        void updateUniformBuffer();
        void destroyGLObjects();

        NISOptimizer* _optimizer;
        NISConfig* _config;
        Mode _mode;
        HDRMode _hdrMode;

        int _inputWidth, _inputHeight;
        int _outputWidth, _outputHeight;
        float _sharpness;

        uint32_t _blockWidth, _blockHeight, _threadGroupSize;
        unsigned int _uboId;       // OpenGL UBO handle
        bool _initialized;

        osg::ref_ptr<osg::Texture2D> _inputTexture;
        osg::ref_ptr<osg::Texture2D> _outputTexture;
        osg::ref_ptr<osg::Texture2D> _coefScalerTexture;
        osg::ref_ptr<osg::Texture2D> _coefUSMTexture;
        osg::ref_ptr<osg::Program> _program;
    };

    /**
     * Result of NISUpscaler::createUpscalingView(): the offscreen camera rendering into
     * 'inputTexture', the upscaler, and the presentation camera showing 'outputTexture'.
     */
    struct NISUpscalingView
    {
        osg::ref_ptr<osg::Camera> sceneCamera;    //!< Pre-render camera drawing into inputTexture
        osg::ref_ptr<osg::Camera> displayCamera;  //!< Post-render camera presenting outputTexture
        osg::ref_ptr<osg::Geometry> displayQuad;  //!< Screen-space quad of the display camera
        osg::ref_ptr<osg::Texture2D> inputTexture, outputTexture;
        osg::ref_ptr<NISUpscaler> upscaler;

        bool valid() const
        {
            return upscaler.valid() && sceneCamera.valid() && displayCamera.valid() &&
                   inputTexture.valid() && outputTexture.valid();
        }
    };

    /**
     * Camera draw callback that auto-dispatches NIS compute shader.
     *
     * Usage in osgVerse Pipeline:
     *   stage->camera->setPreDrawCallback(new NISDrawCallback(nis));
     *
     * Or on any RTT camera where you want NIS to run before that camera renders:
     *   rttCamera->setPreDrawCallback(new NISDrawCallback(nis));
     */
    class NISDrawCallback : public osg::Camera::DrawCallback
    {
    public:
        explicit NISDrawCallback(NISUpscaler* upscaler) : _upscaler(upscaler) {}

        virtual void operator()(osg::RenderInfo& renderInfo) const
        { if (_upscaler.valid()) _upscaler->dispatch(renderInfo.getState()); }

    protected:
        osg::ref_ptr<NISUpscaler> _upscaler;  // keeps the upscaler alive with the camera callback
    };
}

#endif
