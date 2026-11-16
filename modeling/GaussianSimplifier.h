#ifndef MANA_MODELING_GAUSSIANSIMPLIFIER
#define MANA_MODELING_GAUSSIANSIMPLIFIER

#include <functional>
#include <string>
#include <vector>
#include "GaussianGeometry.h"

namespace osgVerse
{

/** Full precision Gaussian splat cloud used by the simplifier.
    - positions / scales: real values (scale is standard deviation, not log)
    - rotations: unit quaternion in (w, x, y, z) order
    - opacities: linear alpha in [0, 1] (not logit)
    - sh: shDim floats per splat, laid out as [dc.rgb, rest] where dc is
      linear RGB (0.5 + SH_C0 * f_dc) and rest coefficients keep their raw
      SH values, ordered as coefficient-major with interleaved RGB
    shDim is 3 (degree 0), 12 (degree 1), 27 (degree 2) or 48 (degree 3).
*/
struct GaussianSplatCloud
{
    GaussianSplatCloud() : count(0), shDim(3) {}

    size_t count;                    // number of splats
    int shDim;                       // 3 / 12 / 27 / 48
    std::vector<osg::Vec3> positions, scales;
    std::vector<osg::Vec4> rotations;  // (w, x, y, z)
    std::vector<float> opacities;
    std::vector<float> sh;

    bool valid() const;
};

/** Options of Gaussian splat simplification, following the NanoGS algorithm */
struct GaussianSimplifyOptions
{
    GaussianSimplifyOptions()
    :   ratio(0.5f), mergeCapRatio(0.5f), opacityThreshold(0.1f),
        kNeighbors(16), sampleCount(1), numThreads(0), maxPasses(32),
        lamGeo(1.0f), lamSh(1.0f), epsCov(1e-8f), seed(0),
        useSHCost(true), verbose(false) {}

    float ratio;             // target splat count / original count, in (0, 1)
    float mergeCapRatio;     // max merges of one pass, relative to original count
    float opacityThreshold;  // splats below min(threshold, median) are pruned
    int kNeighbors;          // k of the kNN graph (excluding the splat itself)
    int sampleCount;         // Monte-Carlo samples per merge cost evaluation
    int numThreads;          // 0 means using OpenMP default thread number
    int maxPasses;           // safety guard of the iteration count

    float lamGeo, lamSh;     // weights of geometry and SH costs
    float epsCov;            // epsilon added to covariance diagonals
    unsigned int seed;       // random seed for Monte-Carlo samples
    bool useSHCost;          // whether SH difference joins the merge cost
    bool verbose;            // print per-pass information with OSG_INFO
};

/** Training-free Gaussian splat simplification:
    it builds a kNN graph over splat centers, estimates the error of merging
    every edge (Monte-Carlo approximation of the mixture cross entropy plus
    a moment-matched entropy term), greedily picks non-overlapping pairs and
    merges them by moment matching. Reference: NanoGS (arXiv 2603.16103).
*/
class GaussianSimplifier
{
public:
    typedef std::function<void(const std::string& stage, size_t current, size_t total)>
        ProgressCallback;

    /** Simplify the cloud in place. Returns false if the input is invalid */
    static bool simplify(GaussianSplatCloud& cloud, const GaussianSimplifyOptions& opt,
                         ProgressCallback callback = ProgressCallback());

    /** Read all splat data of a geometry into a full precision cloud */
    static bool fromGeometry(GaussianGeometry& geom, GaussianSplatCloud& cloud);

    /** Create a new geometry from a simplified cloud */
    static osg::ref_ptr<GaussianGeometry> toGeometry(
        const GaussianSplatCloud& cloud,
        GaussianGeometry::RenderMethod method = GaussianGeometry::INSTANCING);

    /** Convenience: extract, simplify and rebuild a geometry */
    static osg::ref_ptr<GaussianGeometry> simplifyGeometry(
        GaussianGeometry& geom, const GaussianSimplifyOptions& opt,
        ProgressCallback callback = ProgressCallback());
};

}

#endif
