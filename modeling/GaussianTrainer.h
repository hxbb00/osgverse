#ifndef OSGVERSE_MODELING_GAUSSIANTRAINER
#define OSGVERSE_MODELING_GAUSSIANTRAINER

#include <osg/Object>
#include <osg/Matrixf>
#include <osg/ref_ptr>
#include <osg/Vec3f>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "GaussianGeometry.h"

namespace osgVerse
{

/** Input of the 3DGS trainer.
    A COLMAP dataset (or any equivalent SfM result) is parsed by the osgdb_colmap plugin
    and returned as a TrainerInput object:
        osg::ref_ptr<osgVerse::TrainerInput> input =
            osgDB::readObjectFile("colmap:///path/to/dataset");
    Images are not decoded here; only file paths are stored and the trainer loads them.
*/
class TrainerInput : public osg::Object
{
public:
    TrainerInput() : osg::Object(), evalInterval(8), globalScale(1.0f) {}
    TrainerInput(const TrainerInput& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
        : osg::Object(copy, copyop), trainCameras(copy.trainCameras),
          valCameras(copy.valCameras), points(copy.points), pointColors(copy.pointColors),
          evalInterval(copy.evalInterval), globalScale(copy.globalScale) {}
    META_Object(osgVerse, TrainerInput)

    /** COLMAP camera models (the enum values match the official IDs) */
    enum CameraModel
    {
        SIMPLE_PINHOLE = 0, PINHOLE = 1, SIMPLE_RADIAL = 2, RADIAL = 3,
        OPENCV = 4, OPENCV_FISHEYE = 5, FULL_OPENCV = 6, FOV = 7,
        SIMPLE_RADIAL_FISHEYE = 8, RADIAL_FISHEYE = 9,
        THIN_PRISM_FISHEYE = 10, RAD_TAN_THIN_PRISM_FISHEYE = 11
    };

    struct Camera
    {
        Camera() : model(PINHOLE), width(0), height(0), fx(0.0f), fy(0.0f), cx(0.0f), cy(0.0f),
                   k1(0.0f), k2(0.0f), p1(0.0f), p2(0.0f), k3(0.0f), k4(0.0f),
                   k5(0.0f), k6(0.0f), sx1(0.0f), sy1(0.0f) {}

        CameraModel model;
        int width, height;
        float fx, fy, cx, cy;
        float k1, k2, p1, p2, k3, k4, k5, k6, sx1, sy1;
        osg::Matrixf camToWorld;  // camera-to-world transform (OpenCV convention)
        std::string imagePath, maskPath;
    };

    std::vector<Camera> trainCameras;
    std::vector<Camera> valCameras;
    std::vector<osg::Vec3f> points;       // sparse points from SfM
    std::vector<osg::Vec3f> pointColors;  // linear RGB in [0, 1]
    int evalInterval;
    float globalScale;

    int getNumTrainImages() const { return (int)trainCameras.size(); }
    int getNumValImages() const { return (int)valCameras.size(); }
    int getNumPoints() const { return (int)points.size(); }
};

/** Parameters of the 3DGS trainer. Defaults follow VkSplat's default (ADC) strategy.
    Setting useMCMC = true switches to the MCMC strategy and applies its standard
    overrides unless the corresponding values were explicitly changed.
*/
struct GaussianTrainerParameters
{
    GaussianTrainerParameters()
        : deviceId(-1), maxSteps(30000), useMCMC(false), cacheImagesOnGPU(false),
          globalScale(1.0f), initScale(1.0f), initOpacity(0.1f), ssimLambda(0.2f),
          meansLr(1.6e-4f), meansLrFinal(1.6e-6f),
          featuresDcLr(0.0025f), featuresRestLr(0.0025f / 20.0f),
          opacitiesLr(0.05f), scalesLr(0.005f), quatsLr(0.001f),
          scaleReg(0.0f), opacityReg(0.0f),
          refineStartIter(500), refineStopIter(15000), refineEvery(100),
          pruneOpa(0.005f), growGrad2d(0.0002f), growScale3d(0.01f),
          growScale2d(0.05f), pruneScale3d(0.1f), pruneScale2d(0.15f),
          refineScale2dStopIter(0), resetEvery(3000), stopResetAt(-1),
          pauseRefineAfterReset(0), noiseLr(5e5f), minOpacity(0.005f),
          growFactor(1.05f), capMax(1000000) {}

    std::string shaderDir;  // directory containing "slang/", "generated/" and "radix_sort/"
    std::string outputDir;  // optional, for reference only
    int deviceId;           // Vulkan physical device index, -1 for automatic selection
    int maxSteps;
    bool useMCMC;
    bool cacheImagesOnGPU;
    float globalScale;
    float initScale;
    float initOpacity;
    float ssimLambda;
    float meansLr, meansLrFinal;
    float featuresDcLr, featuresRestLr;
    float opacitiesLr, scalesLr, quatsLr;
    float scaleReg, opacityReg;
    int refineStartIter, refineStopIter, refineEvery;
    float pruneOpa, growGrad2d, growScale3d, growScale2d, pruneScale3d, pruneScale2d;
    int refineScale2dStopIter, resetEvery, stopResetAt, pauseRefineAfterReset;
    float noiseLr, minOpacity, growFactor;
    int capMax;
};

/** 3DGS trainer using the vendored VkSplat Vulkan compute backend.
    It runs independently of the OpenGL rendering context; when training finishes,
    createGeometry() reads the parameters back once and returns a GaussianGeometry
    that can be added to any osgVerse scene (no per-step GL readback).
*/
class GaussianTrainer : public osg::Referenced
{
public:
    GaussianTrainer();

    /** Initialize the Vulkan device and upload the dataset.
        Returns false on failure; use getLastError() for a description. */
    bool initialize(const TrainerInput* input, const GaussianTrainerParameters& params);

    /** Run training until maxSteps is reached. */
    bool train();

    /** Run one training iteration. */
    bool step();

    /** Write the current Gaussians to a standard 3DGS PLY file. */
    bool savePLY(const std::string& fileName);

    /** Read parameters back from the GPU and build a GaussianGeometry (INSTANCING). */
    osg::ref_ptr<GaussianGeometry> createGeometry();

    int getCurrentStep() const;
    int getNumSplats() const;
    size_t getVRAMUsage() const;
    size_t getPeakVRAMUsage() const;

    /** Transform applied to the input camera poses and sparse points during training:
        normalizedCamToWorld = dataparserTransform * rawCamToWorld. */
    osg::Matrixf getDataparserTransform() const;

    std::map<std::string, std::tuple<size_t, double>> getTimingBreakdown() const;
    std::string getLastError() const { return _lastError; }

protected:
    virtual ~GaussianTrainer();

    GaussianTrainer(const GaussianTrainer&) = delete;
    GaussianTrainer& operator=(const GaussianTrainer&) = delete;

    struct Impl;
    Impl* _impl;
    std::string _lastError;
};

}  // namespace osgVerse

#endif
