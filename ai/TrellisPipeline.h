#ifndef MANA_AI_TRELLISPIPELINE
#define MANA_AI_TRELLISPIPELINE

#include <osg/Version>
#include <osg/Geometry>
#include <osg/Image>
#include <string>
#include <vector>
#include <functional>

namespace osgVerse
{

    /** Implement Trellis2 functionalities
        std::ifstream f("input.png", std::ios::binary);
        std::vector<unsigned char> imgBytes((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());
        auto mesh = engine->generate(imgBytes, osgVerse::Trellis2::PIPE_AUTO,
                                     osgVerse::Trellis2::BACKGROUND_AUTO,
                                     42, -1, -1.0f, -1,
                                     [](int stage, int step, int total) {
                                         std::cout << "stage " << stage << " : " << step << "/" << total << "\n";
                                     }, nullptr);
        std::cout << "verts: " << mesh->vertices->size() << " tris: " << mesh->triangles->size() / 3
                  << " pbr: " << mesh->hasPBR() << std::endl;

        auto clean = engine->prepareMesh(mesh.get(), 1); // keep largest component
        if (engine->hasPrintRemesh())
        {
            auto watertight = engine->preparePrintMesh(clean.get(), 1, 0.01f, 0.01f / 30.0f);
            auto glb = engine->bakeProjectedGLB(watertight.get(), clean.get(), 2048, 1);
            std::ofstream out("model.glb", std::ios::binary); out.write((char*)glb.data(), glb.size());
        }
     */
    class Trellis2 : public osg::Referenced
    {
    public:
        enum PipelineType
        {
            PIPE_AUTO   = 0,
            PIPE_COARSE = 1,
            PIPE_512    = 2,
            PIPE_1024   = 3
        };

        enum BackgroundMode
        {
            BACKGROUND_AUTO  = 0,
            BACKGROUND_KEEP  = 1,
            BACKGROUND_BLACK = 2,
            BACKGROUND_WHITE = 3
        };

        enum Capabilities
        {
            CAP_COARSE  = 1,
            CAP_512     = 2,
            CAP_1024    = 4,
            CAP_TEXTURE = 8
        };

        enum Stage
        {
            STAGE_PREPROCESS   = 0,
            STAGE_DINO         = 1,
            STAGE_SS_FLOW      = 2,
            STAGE_SS_DEC       = 3,
            STAGE_SLAT_FLOW    = 4,
            STAGE_SHAPE_DEC    = 5,
            STAGE_MESH         = 6,
            STAGE_UPSAMPLE     = 7,
            STAGE_SLAT_FLOW_HR = 8,
            STAGE_SHAPE_DEC_HR = 9,
            STAGE_TEXTURE      = 10
        };

        // data structures
        struct ModelInformation
        {
            std::string backend;
            int caps = 0;          // bitmask of Capabilities
            bool textured = false;
        };

        struct MeshResult : public osg::Referenced
        {
            osg::ref_ptr<osg::Vec3Array> vertices;
            osg::ref_ptr<osg::Vec3Array> normals;
            osg::ref_ptr<osg::DrawElementsUInt> triangles;
            std::vector<float> pbr; // 6*nv: base r/g/b, metallic, roughness, alpha
            bool hasPBR() const { return !pbr.empty(); }
        };

        typedef std::function<void(int stage, int step, int total)> ProgressCallback;
        typedef std::function<void(int stage, int step, int total, const std::vector<unsigned char>& blob)> PreviewCallback;

        // Optional models (pass "" to omit) select available qualities:
        //   - dino                         -> preprocessing
        //   - slatFlow + shapeDec present  -> 512 fine
        //   - (added) slatHR present       -> 1024 cascade
        //   - shapeEnc + texDec + texFlow  -> PBR texturing
        //   - neither pair                 -> coarse preview only
        Trellis2(const std::string& dinoGGUF, const std::string& ssFlowGGUF,
                 const std::string& ssDecGGUF, const std::string& slatFlowGGUF,
                 const std::string& slatHRFlowGGUF, const std::string& shapeDecGGUF,
                 const std::string& shapeEncGGUF = "", const std::string& texDecGGUF = "",
                 const std::string& texFlowGGUF = "", const std::string& texFlowHRGGUF = "",
                 int loadFlags = 0);
        bool checkModelInformation(ModelInformation& info);
        bool isLoaded() const;

        void unload();
        bool reload();

        // Primary: raw encoded image bytes (PNG/JPEG/...; anything stb_image decodes)
        osg::ref_ptr<MeshResult> generate(const std::vector<unsigned char>& imageBytes,
                                          PipelineType pipelineType = PIPE_AUTO,
                                          BackgroundMode backgroundMode = BACKGROUND_AUTO,
                                          uint64_t seed = 0, int steps = -1,
                                          float guidance = -1.0f, int textureSteps = -1,
                                          ProgressCallback onProgress = nullptr,
                                          PreviewCallback onPreview = nullptr);

        // Convenience function with osg::Image (auto-encoded to BMP for the decoder)
        osg::ref_ptr<MeshResult> generate(const osg::Image& image,
                                          PipelineType pipelineType = PIPE_AUTO,
                                          BackgroundMode backgroundMode = BACKGROUND_AUTO,
                                          uint64_t seed = 0, int steps = -1,
                                          float guidance = -1.0f, int textureSteps = -1,
                                          ProgressCallback onProgress = nullptr,
                                          PreviewCallback onPreview = nullptr);

        // Image decode + Trellis.2 preprocessing only (no models). outRGB must hold
        // outSize * outSize * 3 bytes.
        static bool preprocessImage(const std::vector<unsigned char>& imageBytes,
                                    int outSize, std::vector<unsigned char>& outRGB);

        // Mesh post-processing (CPU only; no pipeline required)
        // componentFilter: 0=remove tiny islands, 1=largest only, 2=keep all
        osg::ref_ptr<MeshResult> prepareMesh(const MeshResult* mesh, int componentFilter);

        // Alpha Wrap print remeshing (watertight, 2-manifold)
        osg::ref_ptr<MeshResult> preparePrintMesh(const MeshResult* mesh, int componentFilter,
                                                  float alphaRatio, float offsetRatio);
        bool hasPrintRemesh() const;

        // Bake to UV-atlas GLB (returns the binary blob)
        std::vector<unsigned char> bakeGLB(const MeshResult* mesh,
                                           int textureSize = -1, int componentFilter = 0);

    protected:
        virtual ~Trellis2();
        osg::ref_ptr<osg::Referenced> _internal;
    };

}

#endif
