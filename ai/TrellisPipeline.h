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

    /** Implement Trellis2 pipeline:
        - Image preprocessing
        - DINOv3 encoding (image -> dinodata)
        - Stage-1 SS-flow DiT (dinodata -> z_s latent)
        - Stage-1 SS decoding (z_s latent -> occupancy logit grid)
        - Coarse-mesh creating (z_s latent -> marching cubes)
        - Shape-SLAT DiT (TODO)
        - Shape VAE decoding (TODO)
        - PBR texture-SLAT DiT (TODO)
        - Material sampling (TODO)
     */
    class TrellisPipeline : public osg::Referenced
    {
    public:
        TrellisPipeline();
        bool loadModelsStage1(const std::string& dinoModel, const std::string& ssFlowModel,
                              const std::string& ssDecModel);

    protected:
        virtual ~TrellisPipeline();
        osg::ref_ptr<osg::Referenced> _internal;
    };

}

#endif
