#ifndef MANA_AI_DEPTHANYTHINGENGINE
#define MANA_AI_DEPTHANYTHINGENGINE

#include <osg/Version>
#include <osg/Geometry>
#include <osg/Image>
#include <string>
#include <vector>
#include <functional>

namespace osgVerse
{

    /// Implement Depth-Anything V3 functionalities: depth estimation, gaussian reconstruction
    class DepthAnything : public osg::Referenced
    {
    public:
        DepthAnything(const std::string& model, int numThreads = 0);
        DepthAnything(const std::string& model, const std::string& metricModel, int numThreads = 0);

        struct ModelInformation
        {
            std::string checkpoint, architecture;
            std::string ffn_type;            // "mlp" | "swiglu"
            std::string image_resize_mode;   // "upper_bound" | "lower_bound"
            unsigned int img_resize_target;  // // longest/shortest-side processing resolution
            unsigned int patch_size, embed_dim, depth, num_heads, head_dim, mlp_hidden;
        };
        bool checkModelInfomation(ModelInformation& info);

        struct DepthContent
        {
            osg::ref_ptr<osg::Image> depth, confidence, mono_sky;
            osg::Matrix extrinsics, intrinsics;  // world-to-view
        };
        DepthContent estimateDepth(const osg::Image& input, bool outputPose, bool normalizedResult = true);
        DepthContent estimateDepthMetric(const osg::Image& input, bool normalizedResult = true);
        std::vector<DepthContent> estimateDepths(const std::vector<osg::Image>& inputs, bool normalizedResult = true);
        
        struct GaussianContent
        {
            osg::ref_ptr<osg::Vec3Array> positions, scales;
            osg::ref_ptr<osg::Vec4Array> rotations, reds, greens, blues;
            osg::ref_ptr<osg::FloatArray> alphas;
            osg::Matrix extrinsics, intrinsics;  // world-to-view
        };
        typedef std::function<void (std::vector<float>&, int, int)> DepthResultFunc;
        GaussianContent reconstruct(const osg::Image& input, DepthResultFunc func = NULL);

    protected:
        virtual ~DepthAnything();
        osg::ref_ptr<osg::Referenced> _internal;
    };

}

#endif
