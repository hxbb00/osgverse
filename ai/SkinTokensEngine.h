#ifndef MANA_AI_SKINTOKENSENGINE
#define MANA_AI_SKINTOKENSENGINE

#include <osg/Referenced>
#include <osg/Array>
#include <osg/PrimitiveSet>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Node>
#include <osg/Matrix>
#include <string>
#include <vector>

#include "animation/PlayerAnimation.h"

namespace osgVerse
{

    /** Implement SkinTokens / TokenRig automatic rigging and skinning
        // Build a player (skeleton + learned skin weights) for a static mesh node
        osg::ref_ptr<osgVerse::SkinTokens> engine = new osgVerse::SkinTokens(modelDir, "cpu");
        osg::ref_ptr<osgVerse::SkinTokens::RigResult> result = engine->generateRig(*node);
        osg::ref_ptr<osgVerse::PlayerAnimation> player = engine->createPlayer(*node, *result);
        player->setDrawingSkinning(true);

        // Or keep the data for later use / export
        engine->saveGLB(*result, "character_rigged.glb");
    */
    class SkinTokens : public osg::Referenced
    {
    public:
        SkinTokens(const std::string& model, int numThreads = 0);
        SkinTokens(const std::string& model, const std::string& device, int numThreads = 0);

        struct ModelInformation
        {
            std::string backend;   // GGML backend really used, e.g. "cpu" / "vulkan"
            bool loaded = false;
        };
        bool checkModelInformation(ModelInformation& info);
        bool isLoaded() const;

        struct GenerationOptions
        {
            unsigned long long seed = 0;
            int topK = 5;
            float topP = 0.95f;
            float temperature = 1.0f;
            float repetitionPenalty = 2.0f;
            int beams = 10;
            int maxTokens = 2048;
            bool geometricOnly = false;       // geometry integration baseline only (diagnostic)
            bool surfacePostprocess = false;  // optional upstream voxel_skin heuristic
        };

        /** Rigging result. All vertices, joints and inverse bind poses are expressed in the
            space of the node given to generateRig() (i.e. the node's local space), so the
            result can be used by a PlayerAnimation placed under the same node directly. */
        struct RigResult : public osg::Referenced
        {
            std::vector<PlayerAnimation::JointBinding> joints;
            std::vector<PlayerAnimation::MeshBinding> meshes;
            std::vector<osg::Matrix> inverseBindPoses;  // one per joint

            bool learned = false;   // false means the geometric baseline, not learned weights
            size_t vertexCount = 0, triangleCount = 0;

            /** Opaque holder of the engine-native mesh/skin data, used by saveGLB() */
            osg::ref_ptr<osg::Referenced> nativeData;
        };

        /** Generate a skeleton and learned skin weights for a static mesh scene node.
            All geometries under the given node are merged (their world transforms are baked
            relative to the node) and then rigged as one character. The engine works in a Y-up
            space while osgVerse scenes are Z-up; the node's world transform is taken into
            account for that conversion, so both a Z-up group and the root of a Y-up loaded
            scene are accepted. The result is returned in the node's local space. */
        osg::ref_ptr<RigResult> generateRig(osg::Node& node);
        osg::ref_ptr<RigResult> generateRig(osg::Node& node, const GenerationOptions& options);

        /** Create the PlayerAnimation of a rig result and attach it to the given node, which
            must be an osg::Group. The original geometry is hidden (nodeMask = 0) by default,
            as the animation callback generates its own skinned drawables. The caller has to
            keep the returned player alive (e.g. by its callback reference on the new geode). */
        osg::ref_ptr<PlayerAnimation> createPlayer(osg::Node& node, RigResult& result,
                                                   bool hideSource = true);

        /** Export a self-contained GLB with skeleton, weights and (if any) a rest-pose frame */
        bool saveGLB(const RigResult& result, const std::string& outputFile);

        const std::string& getLastError() const { return _lastError; }

    protected:
        virtual ~SkinTokens();

        osg::ref_ptr<osg::Referenced> _internal;
        std::string _lastError;
    };

}

#endif
