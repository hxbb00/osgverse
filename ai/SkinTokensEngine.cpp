#include "SkinTokensEngine.h"
#include <osg/Notify>
#include <osg/Group>
#include <osg/StateSet>
#include <osg/NodeVisitor>
#include <osg/TriangleIndexFunctor>
#include <algorithm>
#include <cmath>
#include <map>

#include "3rdparty/skintokens/skintokens.hpp"
#include "animation/BlendShapeAnimation.h"

using namespace osgVerse;

namespace
{
    void setError(std::string& target, const std::string& message)
    {
        target = message;
        OSG_WARN << "[SkinTokens] " << message << std::endl;
    }

    skintokens::device_kind parseDevice(const std::string& device)
    {
        if (device == "cpu") return skintokens::device_kind::cpu;
        if (device == "vulkan") return skintokens::device_kind::vulkan;
        return skintokens::device_kind::automatic;
    }

    skintokens::generation_options toGenerationOptions(const SkinTokens::GenerationOptions& o)
    {
        skintokens::generation_options g;
        g.seed = o.seed;
        g.top_k = static_cast<std::uint32_t>(std::max(0, o.topK));
        g.top_p = o.topP;
        g.temperature = o.temperature;
        g.repetition_penalty = o.repetitionPenalty;
        g.beams = static_cast<std::uint32_t>(std::max(0, o.beams));
        g.max_tokens = static_cast<std::size_t>(std::max(0, o.maxTokens));
        g.geometric_only = o.geometricOnly;
        g.surface_postprocess = o.surfacePostprocess;
        return g;
    }

    /** SkinTokens consumes Y-up geometry (glTF convention) while osgVerse scenes are usually
        Z-up, so every vertex passes through this rotation before the rig is generated and the
        resulting joints are rotated back afterwards */
    const osg::Matrix& getZUpToYUp()
    {
        static osg::Matrix m(1.0, 0.0, 0.0, 0.0,
                             0.0, 0.0, -1.0, 0.0,
                             0.0, 1.0, 0.0, 0.0,
                             0.0, 0.0, 0.0, 1.0);
        return m;
    }

    osg::Vec3 toRigSpace(const osg::Vec3& v, const osg::Matrix& toRig)
    { return v * toRig; }

    osg::Vec3 toRigDirection(const osg::Vec3& v, const osg::Matrix& toRig)
    {
        osg::Vec3 r = osg::Matrix::transform3x3(v, toRig);
        r.normalize(); return r;
    }

    /** One geometry collected from the input node, already baked into the node's local space */
    struct CollectedPart
    {
        std::string name;
        osg::ref_ptr<osg::Vec3Array> positions, normals;
        osg::ref_ptr<osg::Vec2Array> uvs;
        osg::ref_ptr<osg::Vec4ubArray> colors;
        std::vector<unsigned int> triangles;
        osg::ref_ptr<osg::StateSet> stateSet;
    };

    struct TriangleCollector
    {
        void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
        {
            if (i1 == i2 || i2 == i3 || i1 == i3) return;
            triangles.push_back(i1); triangles.push_back(i2); triangles.push_back(i3);
        }
        std::vector<unsigned int> triangles;
    };

    class CollectMeshVisitor : public osg::NodeVisitor
    {
    public:
        CollectMeshVisitor(const osg::Matrix& toNode)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN), _toNode(toNode) {}

        virtual void apply(osg::Geode& geode)
        {
            osg::Matrix local = _toNode;
            osg::MatrixList matrices = geode.getWorldMatrices();
            if (!matrices.empty()) local = matrices.front() * _toNode;

            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* geom = geode.getDrawable(i)->asGeometry();
                if (geom != NULL) collect(*geom, local);
            }
            traverse(geode);
        }

        std::vector<CollectedPart>& getParts() { return _parts; }
        size_t getVertexCount() const { return _vertexCount; }
        size_t getTriangleCount() const { return _triangleCount; }

    protected:
        void collect(osg::Geometry& geom, const osg::Matrix& matrix)
        {
            osg::Vec3Array* va = dynamic_cast<osg::Vec3Array*>(geom.getVertexArray());
            if (va == NULL || va->empty()) return;

            CollectedPart part; part.name = geom.getName();
            part.positions = new osg::Vec3Array(va->size());
            for (size_t i = 0; i < va->size(); ++i)
            {
                osg::Vec3 p = (*va)[i];
                if (!matrix.isIdentity()) p = p * matrix;
                (*part.positions)[i] = p;
            }

            osg::Vec3Array* na = dynamic_cast<osg::Vec3Array*>(geom.getNormalArray());
            if (na != NULL && na->size() == va->size())
            {
                part.normals = new osg::Vec3Array(na->size());
                for (size_t i = 0; i < na->size(); ++i)
                {
                    osg::Vec3 n = (*na)[i];
                    if (!matrix.isIdentity()) n = osg::Matrix::transform3x3(n, matrix);
                    n.normalize(); (*part.normals)[i] = n;
                }
            }

            osg::Vec2Array* ta = dynamic_cast<osg::Vec2Array*>(geom.getTexCoordArray(0));
            if (ta != NULL && ta->size() == va->size())
                part.uvs = static_cast<osg::Vec2Array*>(ta->clone(osg::CopyOp::DEEP_COPY_ARRAYS));

            osg::Vec4Array* ca = dynamic_cast<osg::Vec4Array*>(geom.getColorArray());
            if (ca != NULL && ca->size() == va->size())
            {
                part.colors = new osg::Vec4ubArray(ca->size());
                for (size_t i = 0; i < ca->size(); ++i)
                {
                    const osg::Vec4& c = (*ca)[i];
                    (*part.colors)[i] = osg::Vec4ub(
                        (uint8_t)(osg::clampBetween(c[0], 0.0f, 1.0f) * 255.0f),
                        (uint8_t)(osg::clampBetween(c[1], 0.0f, 1.0f) * 255.0f),
                        (uint8_t)(osg::clampBetween(c[2], 0.0f, 1.0f) * 255.0f),
                        (uint8_t)(osg::clampBetween(c[3], 0.0f, 1.0f) * 255.0f));
                }
            }

            osg::TriangleIndexFunctor<TriangleCollector> functor;
            geom.accept(functor);
            part.triangles = functor.triangles;
            if (part.triangles.empty())
            {
                OSG_WARN << "[SkinTokens] Skip geometry without triangles: " << geom.getName()
                         << std::endl; return;
            }

            part.stateSet = geom.getStateSet();
            _vertexCount += part.positions->size();
            _triangleCount += part.triangles.size() / 3;
            _parts.push_back(part);
        }

        osg::Matrix _toNode;
        std::vector<CollectedPart> _parts;
        size_t _vertexCount = 0, _triangleCount = 0;
    };

    class HideMeshVisitor : public osg::NodeVisitor
    {
    public:
        HideMeshVisitor() : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}
        virtual void apply(osg::Geode& geode)
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                if (geode.getDrawable(i)->asGeometry() != NULL)
                { geode.setNodeMask(0); break; }
            }
            traverse(geode);
        }
    };

    /** Engine-native data kept alive by a RigResult, so that results can be exported later */
    struct NativeRigData : public osg::Referenced
    {
        skintokens::mesh mesh;
        skintokens::skin skin;
    };

    class SkinTokensImpl : public osg::Referenced
    {
    public:
        std::unique_ptr<skintokens::model> model;
        std::string backend;
        bool valid = false;

        SkinTokensImpl(const std::string& path, const std::string& device, int numThreads)
        {
            skintokens::runtime_options options;
            options.device = parseDevice(device);
            options.threads = static_cast<std::uint32_t>(std::max(0, numThreads));

            auto loaded = skintokens::model::load(path, options);
            if (!loaded)
            {
                OSG_WARN << "[SkinTokens] Failed to load model: "
                         << loaded.error().message << std::endl; return;
            }

            model.reset(new skintokens::model(std::move(*loaded)));
            backend = std::string(model->backend_name());
            valid = true;
            OSG_NOTICE << "[SkinTokens] Model loaded, backend = " << backend << std::endl;
        }
    };
}

SkinTokens::SkinTokens(const std::string& model, int numThreads)
{
    SkinTokensImpl* impl = new SkinTokensImpl(model, "", numThreads);
    _internal = impl;
    if (!impl->valid) setError(_lastError, "SkinTokens model is not loaded");
}

SkinTokens::SkinTokens(const std::string& model, const std::string& device, int numThreads)
{
    SkinTokensImpl* impl = new SkinTokensImpl(model, device, numThreads);
    _internal = impl;
    if (!impl->valid) setError(_lastError, "SkinTokens model is not loaded");
}

SkinTokens::~SkinTokens()
{}

bool SkinTokens::isLoaded() const
{
    const SkinTokensImpl* impl = static_cast<const SkinTokensImpl*>(_internal.get());
    return impl != NULL && impl->valid;
}

bool SkinTokens::checkModelInformation(ModelInformation& info)
{
    SkinTokensImpl* impl = static_cast<SkinTokensImpl*>(_internal.get());
    info.backend = impl ? impl->backend : std::string();
    info.loaded = isLoaded();
    return info.loaded;
}

osg::ref_ptr<SkinTokens::RigResult> SkinTokens::generateRig(osg::Node& node)
{
    return generateRig(node, GenerationOptions());
}

osg::ref_ptr<SkinTokens::RigResult> SkinTokens::generateRig(osg::Node& node,
                                                            const GenerationOptions& options)
{
    if (!isLoaded())
    {
        if (_lastError.empty()) setError(_lastError, "SkinTokens model is not loaded");
        return NULL;
    }
    SkinTokensImpl* impl = static_cast<SkinTokensImpl*>(_internal.get());

    // 1) Collect all geometry of the node, baked into the node's local space
    osg::Matrix nodeWorld;
    osg::MatrixList nodeMatrices = node.getWorldMatrices();
    if (!nodeMatrices.empty()) nodeWorld = nodeMatrices.front();
    osg::Matrix toNode = nodeWorld.isIdentity() ? osg::Matrix::identity()
                                                : osg::Matrix::inverse(nodeWorld);

    // The engine works in a Y-up space, while osgVerse scenes are Z-up. The node's world
    // transform is taken into account, so a node whose local space is Y-up (e.g. the root of a
    // loaded glTF scene) is handled as well
    osg::Matrix toRig = nodeWorld * getZUpToYUp();
    osg::Matrix fromRig = osg::Matrix::inverse(toRig);

    CollectMeshVisitor cmv(toNode);
    node.accept(cmv);
    std::vector<CollectedPart>& parts = cmv.getParts();
    if (parts.empty())
    {
        setError(_lastError, "No triangle geometry found in the given node");
        return NULL;
    }

    // 2) Merge everything into one Y-up mesh for the engine
    skintokens::mesh rawMesh;
    rawMesh.vertices.reserve(cmv.getVertexCount());
    rawMesh.normals.reserve(cmv.getVertexCount());
    rawMesh.faces.reserve(cmv.getTriangleCount());
    bool withNormals = true;
    std::vector<size_t> partOffsets(parts.size(), 0);
    for (size_t p = 0; p < parts.size(); ++p)
    {
        CollectedPart& part = parts[p];
        partOffsets[p] = rawMesh.vertices.size();
        for (size_t i = 0; i < part.positions->size(); ++i)
        {
            osg::Vec3 v = toRigSpace((*part.positions)[i], toRig);
            rawMesh.vertices.push_back(skintokens::vec3{ v[0], v[1], v[2] });
        }
        if (part.normals.valid())
        {
            for (size_t i = 0; i < part.normals->size(); ++i)
            {
                osg::Vec3 n = toRigDirection((*part.normals)[i], toRig);
                rawMesh.normals.push_back(skintokens::vec3{ n[0], n[1], n[2] });
            }
        }
        else
            withNormals = false;

        for (size_t i = 0; i < part.triangles.size(); i += 3)
        {
            skintokens::triangle tri { (uint32_t)(part.triangles[i + 0] + partOffsets[p]),
                                       (uint32_t)(part.triangles[i + 1] + partOffsets[p]),
                                       (uint32_t)(part.triangles[i + 2] + partOffsets[p]) };
            rawMesh.faces.push_back(tri);
        }
    }
    if (!withNormals) rawMesh.normals.clear();

    // 3) Run the rigging inference
    auto binding = impl->model->rig(rawMesh, toGenerationOptions(options));
    if (!binding)
    {
        setError(_lastError, binding.error().message);
        return NULL;
    }
    const skintokens::skin& skin = *binding;

    osg::ref_ptr<RigResult> result = new RigResult;
    result->learned = skin.learned;
    result->vertexCount = rawMesh.vertices.size();
    result->triangleCount = rawMesh.faces.size();

    // 4) Skeleton: joints are converted back to the node space, rotations are identity because
    //    the engine only provides a rest pose made of joint positions
    const size_t numJoints = skin.rig.names.size();
    std::vector<osg::Vec3> jointPositions(numJoints);
    result->joints.resize(numJoints);
    result->inverseBindPoses.resize(numJoints);
    for (size_t j = 0; j < numJoints; ++j)
    {
        osg::Vec3 p = osg::Vec3(skin.rig.rest_positions[j].x, skin.rig.rest_positions[j].y,
                                skin.rig.rest_positions[j].z) * fromRig;
        jointPositions[j] = p;

        PlayerAnimation::JointBinding& jb = result->joints[j];
        jb.name = skin.rig.names[j];
        jb.parent = j < skin.rig.parents.size() ? skin.rig.parents[j] : -1;
        jb.translation = (jb.parent >= 0) ? (p - jointPositions[(size_t)jb.parent]) : p;
        result->inverseBindPoses[j] = osg::Matrix::translate(-p);
    }

    // 5) Skinned meshes: one PlayerAnimation mesh per collected geometry, so that each part
    //    keeps its own material / texture state set
    result->meshes.resize(parts.size());
    for (size_t p = 0; p < parts.size(); ++p)
    {
        CollectedPart& part = parts[p];
        PlayerAnimation::MeshBinding& mb = result->meshes[p];
        mb.name = part.name.empty() ? ("Part" + std::to_string(p)) : part.name;
        mb.positions = part.positions;
        mb.normals = part.normals;
        mb.uvs = part.uvs;
        mb.colors = part.colors;
        mb.stateSet = part.stateSet;
        mb.inverseBindPoses = result->inverseBindPoses;

        const size_t vBase = partOffsets[p], vCount = mb.positions->size();
        if (vCount > 65535)  // OZZ mesh indices are 16-bit
        {
            OSG_WARN << "[SkinTokens] Part " << mb.name << " has " << vCount
                     << " vertices which exceeds the OZZ 16-bit index limit" << std::endl;
        }

        mb.jointIndices.resize(vCount * 4);
        mb.jointWeights.resize(vCount * 4);
        for (size_t v = 0; v < vCount; ++v)
        {
            const size_t src = vBase + v;
            float weights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            uint16_t indices[4] = { 0, 0, 0, 0 };
            if (src < skin.weights.size())
            {
                for (int k = 0; k < 4; ++k) weights[k] = std::max(0.0f, skin.weights[src][(size_t)k]);
            }
            if (src < skin.joints.size())
            {
                for (int k = 0; k < 4; ++k)
                {
                    uint16_t id = skin.joints[src][(size_t)k];
                    indices[k] = (id < numJoints) ? id : (uint16_t)0;
                }
            }

            float sum = weights[0] + weights[1] + weights[2] + weights[3];
            if (sum <= 0.0f) { weights[0] = 1.0f; sum = 1.0f; }
            for (int k = 0; k < 4; ++k)
            {
                mb.jointIndices[v * 4 + k] = indices[k];
                mb.jointWeights[v * 4 + k] = weights[k] / sum;
            }
        }

        mb.indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        mb.indices->reserve(part.triangles.size());
        for (size_t i = 0; i < part.triangles.size(); ++i)
            mb.indices->push_back(part.triangles[i]);
    }

    NativeRigData* native = new NativeRigData;
    native->mesh = rawMesh; native->skin = skin;
    result->nativeData = native;
    OSG_NOTICE << "[SkinTokens] Rig generated: " << numJoints << " joints, "
               << result->vertexCount << " vertices, " << result->triangleCount << " triangles"
               << (result->learned ? "" : " (geometric baseline)") << std::endl;
    return result;
}

osg::ref_ptr<PlayerAnimation> SkinTokens::createPlayer(osg::Node& node, RigResult& result,
                                                       bool hideSource)
{
    osg::Group* group = node.asGroup();
    if (group == NULL)
    {
        setError(_lastError, "The given node must be a group to hold the generated character");
        return NULL;
    }
    if (result.joints.empty() || result.meshes.empty())
    {
        setError(_lastError, "The rig result is empty");
        return NULL;
    }

    osg::ref_ptr<PlayerAnimation> player = new PlayerAnimation;
    player->setName("SkinTokensPlayer");
    player->setModelRoot(&node);
    if (!player->initialize(result.joints, result.meshes))
    {
        setError(_lastError, "Failed to initialize PlayerAnimation from the rig result");
        return NULL;
    }

    if (hideSource)
    {
        HideMeshVisitor hmv; node.accept(hmv);
    }

    // The animation callback generates its own skinned drawables on this geode, which lives
    // in the same local space as the rig result
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->setName("SkinTokensCharacter");
    geode->setUpdateCallback(player.get());
    group->addChild(geode.get());
    return player;
}

bool SkinTokens::saveGLB(const RigResult& result, const std::string& outputFile)
{
    NativeRigData* native = dynamic_cast<NativeRigData*>(result.nativeData.get());
    if (native == NULL || native->skin.rig.names.empty())
    {
        setError(_lastError, "The rig result has no native data to export");
        return false;
    }

    // The collected mesh is already in the Y-up rigging space used by SkinTokens
    skintokens::motion rest;
    rest.frames = 1;
    rest.frames_per_second = 30.0f;
    rest.rig = native->skin.rig;
    const skintokens::vec3 root = rest.rig.rest_positions.empty()
                                ? skintokens::vec3() : rest.rig.rest_positions.front();
    rest.root_translations.assign(1, root);
    rest.local_rotations.assign(rest.rig.names.size(), skintokens::quat());

    auto saved = skintokens::save_skinned_animation_glb_file(
        outputFile, native->mesh, native->skin, rest);
    if (!saved)
    {
        setError(_lastError, saved.error().message);
        return false;
    }
    return true;
}
