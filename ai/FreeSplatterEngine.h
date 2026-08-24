#ifndef MANA_AI_FREESPLATTERENGINE
#define MANA_AI_FREESPLATTERENGINE

#include <osg/Referenced>
#include <osg/Image>
#include <osg/Array>
#include <osg/Matrix>
#include <osg/Quat>
#include <string>
#include <vector>

namespace osgVerse
{

    /** Implement Free-Splatter functionalities
        // Gaussians from single/multiple images
        std::vector<osg::ref_ptr<osg::Image>> imgs
        auto g = engine->estimateGaussians(imgs);
        auto poses = engine->estimatePoses(g);  // solve poses
        g.unpack(5e-3f);                        // fill OSG arrays
        osgVerse::FreeSplatter::writeSplat(fused, "out.splat");

        // Gaussians from accumulated video data
        osgVerse::FreeSplatter::Accumulator acc(512, 512, 5e-3f);
        acc.addPair(pairGaussians);          // assert(n_views == 2)
        auto cloud = acc.getCloud();         // Original point cloud
        auto fused = acc.fuse(0.02f, 2, 1);  // Fused point cloud
        osgVerse::FreeSplatter::writeSplat(fused, "out.splat", 200000, 1.0f);
    */
    class FreeSplatter : public osg::Referenced
    {
    public:
        FreeSplatter(const std::string& model, int numThreads = 0);
        FreeSplatter(const std::string& model, const std::string& device, int numThreads = 0);

        struct ModelInformation
        {
            int image_width, image_height;
            int in_channels, gaussian_channels;
        };
        bool checkModelInformation(ModelInformation& info);

        // Gaussian parameter output (per-pixel, render-ready)
        struct GaussianContent
        {
            // Raw engine output: n_views * height * width * gaussian_channels float32
            std::vector<float> raw;
            int n_views = 0, height = 0, width = 0, gaussian_channels = 0;

            // Unpacked OSG arrays (populated on demand by unpack())
            osg::ref_ptr<osg::Vec3Array> positions;
            osg::ref_ptr<osg::Vec3Array> scales;
            osg::ref_ptr<osg::Vec4Array> rotations;   // (w, x, y, z)
            osg::ref_ptr<osg::Vec3Array> colors;      // activated RGB [0,1]
            osg::ref_ptr<osg::FloatArray> opacities;
            void unpack(float opacityThreshold = 5e-3f);
        };

        // Run inference on one or more views. Images are center-cropped to a square
        // and resized to the model resolution automatically.
        GaussianContent estimateGaussians(osg::Image* input);
        GaussianContent estimateGaussians(const std::vector<osg::ref_ptr<osg::Image>>& inputs);

        // Load from a raw float dump (the .f32 files the CLI writes).
        static GaussianContent loadFromRaw(const std::string& path, int n_views,
                                           int height, int width, int gc);

        // Pose recovery
        std::vector<osg::Matrixf> estimatePoses(const GaussianContent& g,
                                                float opacityThreshold = 5e-3f,
                                                float* sharedFocal = nullptr);

        // Parallax / depth conditioning
        struct ParallaxContent
        {
            float tri_angle_deg = 0.0f;
            float lateral_angle_deg = 0.0f;
            float baseline_over_depth = 0.0f;
            float baseline = 0.0f;
            float median_depth = 0.0f;
            float focal = 0.0f;
            int n_points = 0;
        };
        ParallaxContent estimateParallax(const GaussianContent& g,
                                         float opacityThreshold = 5e-3f);

        // Sliding-window accumulator (video/stream mode)
        class Accumulator : public osg::Referenced
        {
        public:
            Accumulator(int width, int height, float opacityThreshold = 5e-3f);
            int getFrameCount() const;

            // Add a pair (2 views). View-0 must be the same frame as the previous
            // pair's view-1 (overlap-by-one chaining).
            bool addPair(const GaussianContent& pairGaussians);

            struct Point
            {
                osg::Vec3f position;
                osg::Vec3f color;
                float opacity = 1.0f;
                osg::Vec3f scale;
                osg::Quat rotation;
                int frame = 0;
            };

            std::vector<Point> getCloud() const;
            std::vector<Point> fuse(float voxelFrac = 0.02f, int k = 2, int mode = 1) const;
            bool refine(float voxelFrac = 0.03f, int iters = 8, float alpha = 0.5f);
            std::vector<osg::Matrixf> getCameraPath() const;

        protected:
            virtual ~Accumulator();
            osg::ref_ptr<osg::Referenced> _internal;
        };

        // Hierarchical tree accumulation (batch mode)
        struct TreeOptions
        {
            int block = 4;          // submap width in frames
            int overlap = 2;        // shared frames between adjacent submaps
            int maxLevels = -1;     // -1 = full merge
            int perNodeCap = 0;     // 0 = unlimited
            float layoutSpacing = 0.0f; // 0=none, <0=auto, >0=explicit
        };

        // Each GaussianContent in `pairs` must contain exactly 2 views.
        static std::vector<Accumulator::Point> treeOverlap(
                const std::vector<GaussianContent>& pairs,
                int width, int height, float opacityThreshold, const TreeOptions& opts);

        // I/O utilities
        static bool writeSplat(const std::vector<Accumulator::Point>& points, const std::string& path,
                               size_t maxSplats = 0, float scaleMult = 1.0f);
        static bool writeSplat(const GaussianContent& g, const std::string& path,
                               float opacityThreshold = 5e-3f, size_t maxSplats = 0);
        static bool writeRaw(const GaussianContent& g, const std::string& path);

        // Image preprocessing (center-crop -> square -> resize -> CHW [0,1])
        static std::vector<float> preprocessImage(const osg::Image& img, int targetSize);

    protected:
        virtual ~FreeSplatter();
        osg::ref_ptr<osg::Referenced> _internal;
    };

}

#endif
