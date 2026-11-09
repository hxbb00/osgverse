#include "PlayerAnimation.h"
#include "PlayerAnimationInternal.h"
#include "BlendShapeAnimation.h"
#include "modeling/Math.h"
#include "pipeline/Utilities.h"
#include <osg/io_utils>
#include <osg/Version>
#include <osg/MatrixTransform>
#include <osg/TriangleIndexFunctor>
#include <osgDB/ReadFile>
#include <nanoid/nanoid.h>
#include <algorithm>
#include <iomanip>

using namespace osgVerse;
using namespace ozz::animation::internal;
#define SK_ERROR(msg) { OSG_WARN << "[PlayerAnimation] " << msg << std::endl; }

static osg::Texture2D* createTexture(const std::string& fileName)
{
    osg::ref_ptr<osg::Texture2D> tex = new osg::Texture2D;
    tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP);
    tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP);
    tex->setImage(osgDB::readImageFile(fileName));
    return tex.release();
}

namespace ozz
{
    namespace animation
    {
        class CreateSkeletonVisitor : public osg::NodeVisitor
        {
        public:
            CreateSkeletonVisitor()
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}
            const std::vector<osg::Transform*>& getSkeletonNodes() const { return _nodeList; }

            virtual void apply(osg::Transform& node)
            {
                if (node.getName().empty())
                {
                    OSG_WARN << "[PlayerAnimation] Find possible skeleton node without name. "
                             << "Will set a default one but will not work with animations." << std::endl;
                    node.setName("Joint_" + nanoid::generate(8));
                }

                if (node.getNumParents() > 0)
                {
                    if (_parentMap.find(&node) == _parentMap.end())
                    {
                        _parentMap[&node] = node.getParent(0);
                        _nodeList.push_back(&node);
                        _namesToStore.append(node.getName() + " ");
                        _namesToStore.back() = '\0';
                    }
                }
                traverse(node);
            }

            void initialize(const std::vector<osg::Transform*>& skeletonList)
            {   // call this if not use me as a visitor
                for (size_t i = 0; i < skeletonList.size(); ++i)
                {
                    osg::Transform* node = skeletonList[i];
                    if (node->getNumParents() > 0)
                        _parentMap[node] = node->getParent(0);
                    _namesToStore.append(node->getName() + " ");
                    _namesToStore.back() = '\0';
                }
                _nodeList = skeletonList;
            }

            void build(ozz::animation::Skeleton& skeleton)
            {
                int32_t charsCount = (int32_t)_namesToStore.size();
                int32_t numJoints = (int32_t)_nodeList.size();
                skeleton.Deallocate(); if (numJoints < 1) return;

                char* cursor = skeleton.Allocate(charsCount, numJoints);
                memcpy(cursor, _namesToStore.data(), charsCount);
                for (int i = 0; i < numJoints - 1; ++i)
                {
                    skeleton.joint_names_[i] = cursor;
                    cursor += std::strlen(skeleton.joint_names_[i]) + 1;
                }
                skeleton.joint_names_[numJoints - 1] = cursor;

                osg::Matrix matrices[4]; int soaIndex = 0;
                for (int i = 0; i < numJoints; ++i)
                {
                    osg::Transform* t = _nodeList[i];
                    skeleton.joint_parents_[i] = -1;
                    if (_parentMap.find(t) != _parentMap.end())
                    {
                        osg::Transform* parent = dynamic_cast<osg::Transform*>(_parentMap.find(t)->second);
                        if (parent != NULL)
                        {
                            std::vector<osg::Transform*>::iterator itr =
                                std::find(_nodeList.begin(), _nodeList.end(), parent);
                            if (itr != _nodeList.end())
                                skeleton.joint_parents_[i] = std::distance(_nodeList.begin(), itr);
                            //else
                            //    SK_ERROR("Parent node (" << parent->getName() << ") of " << t->getName() << " not in NodeList")
                        }
                    }
                    //else
                    //    SK_ERROR("No parent found for " << t->getName() << ", ID = " << i)

                    matrices[soaIndex].makeIdentity();
                    t->computeLocalToWorldMatrix(matrices[soaIndex], NULL);
                    //matrices[soaIndex] = t->getWorldMatrices()[0];
                    if ((++soaIndex) == 4)
                    { applySoaTransform(skeleton, matrices, i / 4, soaIndex); soaIndex = 0; }
                }
                if (soaIndex != 0)
                    applySoaTransform(skeleton, matrices, (numJoints + 3) / 4 - 1, soaIndex);
            }

            void build(ozz::animation::Skeleton& skeleton,
                       const std::vector<osgVerse::PlayerAnimation::JointBinding>& joints)
            {   // Build skeleton from index based binding, no scene graph node required
                skeleton.Deallocate(); int32_t numJoints = (int32_t)joints.size();
                if (numJoints < 1) return;

                size_t charsCount = 1;
                for (int i = 0; i < numJoints; ++i) charsCount += joints[i].name.size() + 1;
                char* cursor = skeleton.Allocate((int32_t)charsCount, numJoints);
                for (int i = 0; i < numJoints; ++i)
                {
                    const std::string& name = joints[i].name;
                    skeleton.joint_names_[i] = cursor;
                    memcpy(cursor, name.c_str(), name.size()); cursor += name.size();
                    *cursor++ = '\0';
                    skeleton.joint_parents_[i] = (int16_t)joints[i].parent;
                }

                osg::Matrix matrices[4]; int soaIndex = 0;
                for (int i = 0; i < numJoints; ++i)
                {
                    matrices[soaIndex] = joints[i].toMatrix();
                    if ((++soaIndex) == 4)
                    { applySoaTransform(skeleton, matrices, i / 4, soaIndex); soaIndex = 0; }
                }
                if (soaIndex != 0)
                    applySoaTransform(skeleton, matrices, (numJoints + 3) / 4 - 1, soaIndex);
            }

        protected:
            void applySoaTransform(ozz::animation::Skeleton& skeleton, osg::Matrix* matrices,
                                   int idx, int numSoa)
            {
                osg::Vec3 p[4], s[4]; osg::Quat q[4], so;
                for (int j = 0; j < numSoa; ++j) matrices[j].decompose(p[j], q[j], s[j], so);
                for (int j = numSoa; j < 4; ++j) s[j] = osg::Vec3(1.0f, 1.0f, 1.0f);

                skeleton.joint_rest_poses_[idx] = ozz::math::SoaTransform{
                    ozz::math::SoaFloat3 {
                        ozz::math::simd_float4::Load(p[0].x(), p[1].x(), p[2].x(), p[3].x()),
                        ozz::math::simd_float4::Load(p[0].y(), p[1].y(), p[2].y(), p[3].y()),
                        ozz::math::simd_float4::Load(p[0].z(), p[1].z(), p[2].z(), p[3].z()) },
                    ozz::math::SoaQuaternion {
                        ozz::math::simd_float4::Load(q[0].x(), q[1].x(), q[2].x(), q[3].x()),
                        ozz::math::simd_float4::Load(q[0].y(), q[1].y(), q[2].y(), q[3].y()),
                        ozz::math::simd_float4::Load(q[0].z(), q[1].z(), q[2].z(), q[3].z()),
                        ozz::math::simd_float4::Load(q[0].w(), q[1].w(), q[2].w(), q[3].w()) },
                    ozz::math::SoaFloat3 {
                        ozz::math::simd_float4::Load(s[0].x(), s[1].x(), s[2].x(), s[3].x()),
                        ozz::math::simd_float4::Load(s[0].y(), s[1].y(), s[2].y(), s[3].y()),
                        ozz::math::simd_float4::Load(s[0].z(), s[1].z(), s[2].z(), s[3].z()) } };
            }

            std::map<osg::Transform*, osg::Node*> _parentMap;
            std::vector<osg::Transform*> _nodeList;
            std::string _namesToStore;
        };

        struct CollectTriangleOperator
        {
            void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
            {
                if (i1 == i2 || i2 == i3 || i1 == i3) return;
                triangles.push_back(startIndex + i1);
                triangles.push_back(startIndex + i2);
                triangles.push_back(startIndex + i3);
            }

            std::vector<uint16_t> triangles;
            uint16_t startIndex;
        };

        class CreateMeshVisitor : public osg::NodeVisitor
        {
        public:
            CreateMeshVisitor(const std::vector<osg::Transform*>& nodes,
                              const std::map<osg::Geometry*, PlayerAnimation::GeometryJointData>& jd)
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN), _jointMap(jd)
            { for (size_t i = 0; i < nodes.size(); ++i) _nodeMap[nodes[i]] = i; }

            ozz::vector<OzzMesh>& getMeshes(ozz::vector<std::string>& n) { n = _meshNames; return _meshes; }
            std::vector<osg::ref_ptr<osgVerse::BlendShapeAnimation>>& getBS() { return _blendshapes; }
            std::vector<osg::ref_ptr<osg::StateSet>>& getStateSets() { return _stateSetList; }

            virtual void apply(osg::Geode& node)
            {
#if OSG_VERSION_LESS_OR_EQUAL(3, 4, 1)
                for (size_t i = 0; i < node.getNumDrawables(); ++i)
                {
                    osg::Geometry* geom = node.getDrawable(i)->asGeometry();
                    if (geom && _jointMap.find(geom) != _jointMap.end())
                        apply(*geom, _jointMap.find(geom)->second);
                }
#endif
                traverse(node);
            }

            virtual void apply(osg::Geometry& geom)
            {
#if OSG_VERSION_GREATER_THAN(3, 4, 1)
                if (_jointMap.find(&geom) != _jointMap.end())
                    apply(geom, _jointMap.find(&geom)->second);
                traverse(geom);
#endif
            }

            void initialize(const std::vector<osg::Geometry*>& meshList)
            {   // call this if not use me as a visitor
                for (size_t i = 0; i < meshList.size(); ++i)
                {
                    osg::Geometry* geom = meshList[i];
                    if (geom && _jointMap.find(geom) != _jointMap.end())
                        apply(*geom, _jointMap.find(geom)->second);
                }
            }

            void apply(osg::Geometry& geom, PlayerAnimation::GeometryJointData& jData)
            {
                osg::Vec3Array* va = static_cast<osg::Vec3Array*>(geom.getVertexArray());
                osg::Vec3Array* na = static_cast<osg::Vec3Array*>(geom.getNormalArray());
                osg::Vec4Array* ta = dynamic_cast<osg::Vec4Array*>(geom.getVertexAttribArray(6));
                osg::Vec4Array* ca = dynamic_cast<osg::Vec4Array*>(geom.getColorArray());
                osg::Vec2Array* uv = dynamic_cast<osg::Vec2Array*>(geom.getTexCoordArray(0));
                if (!va || (va && va->empty())) return;

                size_t vCount = va->size(), wCount = jData._weightList.size();
                if (vCount > SIZE_MAX / (sizeof(float) * 4) || vCount != wCount)
                {
                    OSG_WARN << "[PlayerAnimation] Imported joint-weight list size mismatched: "
                             << wCount << "; vertex count: " << vCount << std::endl; return;
                }

                // Get and apply transformation matrix of current geometry. Vertices and the OZZ
                // skeleton must live in the same "character space" (parent space of the root
                // joint), so the per-geometry world matrix is pre-multiplied with _toCharacter.
                osg::Matrix worldMatrix = jData._toCharacter;  // OSG uses row vectors
                if (geom.getNumParents() > 0)
                    worldMatrix = geom.getParent(0)->getWorldMatrices()[0] * worldMatrix;
                const bool transformGeometry = !worldMatrix.isIdentity();

                // A rig may have been skinned in a bind pose different from its node rest pose.
                // OZZ composes the skeleton from the rest pose, so the vertices have to be
                // retargeted from bind to rest first (per vertex, along its strongest joint).
                // The delta is identity for the usual case of bind == rest.
                std::vector<const osg::Matrixf*> vDeltas(vCount, (const osg::Matrixf*)NULL);
                bool hasDelta = false;
                for (size_t i = 0; i < wCount && i < vCount && !jData._bindToRestMap.empty(); ++i)
                {
                    const std::vector<std::pair<osg::Transform*, float>>& jMap = jData._weightList[i];
                    const osg::Matrixf* best = NULL; float bestWeight = 0.0f;
                    for (size_t k = 0; k < jMap.size(); ++k)
                    {
                        if (jMap[k].second <= bestWeight) continue;
                        std::map<osg::Transform*, osg::Matrixf>::iterator dItr =
                            jData._bindToRestMap.find(jMap[k].first);
                        if (dItr == jData._bindToRestMap.end()) continue;
                        bestWeight = jMap[k].second; best = &(dItr->second);
                    }
                    if (best != NULL && !best->isIdentity())
                        { vDeltas[i] = best; hasDelta = true; }
                }

                // Apply to mesh part
                OzzMesh::Part meshPart; meshPart.positions.resize(vCount * 3);
                if (transformGeometry || hasDelta)
                {
                    for (size_t c = 0; c < vCount; ++c)
                    {
                        osg::Vec3 v = (*va)[c];
                        if (transformGeometry) v = v * worldMatrix;
                        if (vDeltas[c] != NULL) v = v * (*(vDeltas[c]));
                        size_t idx = c * 3;
                        for (int k = 0; k < 3; ++k) meshPart.positions[idx + k] = v[k];
                    }
                }
                else
                    memcpy(&meshPart.positions[0], &(*va)[0], vCount * sizeof(float) * 3);

                if (ta && ta->size() == vCount)
                {
                    meshPart.tangents.resize(vCount * 4);
                    for (size_t c = 0; c < vCount; ++c)
                    {
                        const osg::Vec4& t = (*ta)[c];
                        osg::Vec3 xyz(t.x(), t.y(), t.z());
                        if (transformGeometry) xyz = osg::Matrix::transform3x3(xyz, worldMatrix);
                        if (vDeltas[c] != NULL) xyz = osg::Matrix::transform3x3(xyz, *(vDeltas[c]));
                        size_t idx = c * 4;
                        meshPart.tangents[idx + 0] = xyz[0]; meshPart.tangents[idx + 1] = xyz[1];
                        meshPart.tangents[idx + 2] = xyz[2]; meshPart.tangents[idx + 3] = t.w();
                    }
                }

                if (na && na->size() == vCount)
                {
                    meshPart.normals.resize(vCount * 3);
                    for (size_t c = 0; c < vCount; ++c)
                    {
                        osg::Vec3 n = (*na)[c];
                        if (transformGeometry) n = osg::Matrix::transform3x3(n, worldMatrix);
                        if (vDeltas[c] != NULL) n = osg::Matrix::transform3x3(n, *(vDeltas[c]));
                        n.normalize(); size_t idx = c * 3;
                        for (int k = 0; k < 3; ++k) meshPart.normals[idx + k] = n[k];
                    }
                }
                if (ca && ca->size() == vCount)
                {
                    meshPart.colors.resize(vCount * 4);
                    for (size_t c = 0; c < vCount; ++c)
                    {
                        const osg::Vec4& color = (*ca)[c]; size_t idx = c * 4;
                        for (size_t k = 0; k < 4; ++k)
                            meshPart.colors[idx + k] = (uint8_t)(color[k] * 255.0f);
                    }
                }
                if (uv && uv->size() == vCount)
                {
                    meshPart.uvs.resize(vCount * 2);
                    memcpy(&meshPart.uvs[0], &(*uv)[0], vCount * sizeof(float) * 2);
                }

                // Compute joint weights. A vertex can have 1..4 influences in glTF/FBX, and OZZ
                // expects exactly 4 joint indices with 3 weights per vertex (the 4th weight is
                // implicitly 1 - sum). Silently merging the last two joints (the previous
                // behaviour) deformed any 4-influence vertex incorrectly.
                for (size_t i = 0; i < wCount; ++i)
                {
                    std::vector<std::pair<osg::Transform*, float>>& jMap = jData._weightList[i];
                    std::vector<std::pair<uint16_t, float>> influences;
                    influences.reserve(4);
                    for (size_t k = 0; k < jMap.size(); ++k)
                    {
                        osg::Transform* joint = jMap[k].first; float weight = jMap[k].second;
                        if (weight <= 0.0f) continue;

                        std::map<osg::Transform*, uint16_t>::iterator itr = _nodeMap.find(joint);
                        if (itr == _nodeMap.end())
                        {
                            OSG_WARN << "[PlayerAnimation] Invalid joint linked to mesh: "
                                     << (joint ? joint->getName() : std::string("NULL"))
                                     << std::endl; continue;
                        }
                        influences.push_back(std::pair<uint16_t, float>(itr->second, weight));
                    }

                    // Keep the strongest 4 influences, then normalize them
                    if (influences.size() > 4)
                    {
                        std::sort(influences.begin(), influences.end(),
                                  [](const std::pair<uint16_t, float>& a,
                                     const std::pair<uint16_t, float>& b)
                                  { return a.second > b.second; });
                        influences.resize(4);
                    }
                    float weightSum = 0.0f;
                    for (size_t k = 0; k < influences.size(); ++k) weightSum += influences[k].second;
                    for (size_t k = 0; k < 4; ++k)
                    {
                        uint16_t joint = (k < influences.size()) ? influences[k].first : 0;
                        float weight = (k < influences.size() && weightSum > 0.0f)
                                     ? influences[k].second / weightSum : 0.0f;
                        meshPart.joint_indices.push_back(joint);
                        if (k < 3) meshPart.joint_weights.push_back(weight);
                    }
                }

#if false
                int step0 = numJointsToWeight, step1 = (numJointsToWeight - 1), id0 = 0, id1 = 0;
                for (; id0 < meshPart.joint_indices.size(), id1 < meshPart.joint_weights.size();
                       id0 += step0, id1 += step1)
                {
                    std::cout << "ID-" << (id0 / step0) << ": ";
                    for (int k = 0; k < step1; ++k)
                        std::cout << "J" << meshPart.joint_indices[id0 + k]
                                  << " = " << meshPart.joint_weights[id1 + k] << "; ";
                    std::cout << "J" << meshPart.joint_indices[id0 + step1] << std::endl;
                }
                std::cout << "... VERTICES " << wCount << ", J = " << meshPart.joint_indices.size()
                          << ", W = " << meshPart.joint_weights.size() << std::endl;
#endif

                // Handle blendshapes. The morph deltas are stored in mesh local space by the
                // importers, but the vertices are baked into the character space (which includes
                // e.g. the Y-up to Z-up rotation and the skeleton-root parent transform). Without
                // rotating the deltas too, a jaw/eye displacement would move along the wrong
                // axis (usually depth), making eyes z-fight and flicker.
                BlendShapeAnimation* bsa =
                    dynamic_cast<osgVerse::BlendShapeAnimation*>(geom.getUpdateCallback());
                _blendshapes.push_back(bsa != NULL ? bsa : NULL);
                if (bsa != NULL && transformGeometry)
                {
                    std::vector<osg::ref_ptr<BlendShapeAnimation::BlendShapeData>>& allShapes =
                        bsa->getAllBlendShapes();
                    for (size_t s = 0; s < allShapes.size(); ++s)
                    {
                        BlendShapeAnimation::BlendShapeData* bsd = allShapes[s].get();
                        if (bsd->vertices.valid())
                        {
                            osg::Vec3Array& d = *(bsd->vertices);
                            for (size_t v = 0; v < d.size(); ++v)
                                d[v] = osg::Matrix::transform3x3(d[v], worldMatrix);
                        }
                        if (bsd->normals.valid())
                        {
                            osg::Vec3Array& d = *(bsd->normals);
                            for (size_t v = 0; v < d.size(); ++v)
                                d[v] = osg::Matrix::transform3x3(d[v], worldMatrix);
                        }
                        if (bsd->tangents.valid())
                        {
                            osg::Vec4Array& d = *(bsd->tangents);
                            for (size_t v = 0; v < d.size(); ++v)
                            {
                                osg::Vec3 xyz = osg::Matrix::transform3x3(
                                    osg::Vec3(d[v].x(), d[v].y(), d[v].z()), worldMatrix);
                                d[v].set(xyz[0], xyz[1], xyz[2], d[v].w());
                            }
                        }
                    }
                }

                // Apply to OZZ mesh
                osg::TriangleIndexFunctor<CollectTriangleOperator> functor;
                functor.startIndex = 0; geom.accept(functor);

                OzzMesh mesh; mesh.parts.push_back(meshPart);
                mesh.triangle_indices.assign(functor.triangles.begin(), functor.triangles.end());

                // OZZ requires joint remaps to be sorted, and inverse bind poses must follow the
                // same order. Joints are resolved with find() so an invalid node never becomes
                // joint 0 silently.
                std::vector<std::pair<uint16_t, osg::Matrixf>> invBinds;
                invBinds.reserve(jData._invBindPoseMap.size());
                for (std::map<osg::Transform*, osg::Matrixf>::iterator itr =
                     jData._invBindPoseMap.begin(); itr != jData._invBindPoseMap.end(); ++itr)
                {
                    std::map<osg::Transform*, uint16_t>::iterator idItr = _nodeMap.find(itr->first);
                    if (idItr == _nodeMap.end())
                    {
                        OSG_WARN << "[PlayerAnimation] Invalid joint in inverse bind pose map: "
                                 << (itr->first ? itr->first->getName() : std::string("NULL"))
                                 << std::endl; continue;
                    }
                    invBinds.push_back(std::pair<uint16_t, osg::Matrixf>(idItr->second, itr->second));
                }

                std::sort(invBinds.begin(), invBinds.end(),
                          [](const std::pair<uint16_t, osg::Matrixf>& a,
                             const std::pair<uint16_t, osg::Matrixf>& b)
                          { return a.first < b.first; });
                for (size_t j = 0; j < invBinds.size(); ++j)
                {
                    mesh.joint_remaps.push_back(invBinds[j].first);
                    const osg::Matrixf& m = invBinds[j].second;
                    ozz::math::Float4x4 pose = ozz::math::Float4x4::identity();
                    pose.cols[0] = ozz::math::simd_float4::Load(m(0, 0), m(0, 1), m(0, 2), m(0, 3));
                    pose.cols[1] = ozz::math::simd_float4::Load(m(1, 0), m(1, 1), m(1, 2), m(1, 3));
                    pose.cols[2] = ozz::math::simd_float4::Load(m(2, 0), m(2, 1), m(2, 2), m(2, 3));
                    pose.cols[3] = ozz::math::simd_float4::Load(m(3, 0), m(3, 1), m(3, 2), m(3, 3));
                    mesh.inverse_bind_poses.push_back(pose);
                }
                _meshes.push_back(mesh); _meshNames.push_back(std::string(geom.getName()));
                _stateSetList.push_back(jData._stateset);
            }

        protected:
            std::map<osg::Geometry*, PlayerAnimation::GeometryJointData> _jointMap;
            std::map<osg::Transform*, uint16_t> _nodeMap;
            std::vector<osg::ref_ptr<osg::StateSet>> _stateSetList;
            ozz::vector<OzzMesh> _meshes; ozz::vector<std::string> _meshNames;
            std::vector<osg::ref_ptr<osgVerse::BlendShapeAnimation>> _blendshapes;
        };

        class AnimationConverter
        {
        public:
            AnimationConverter() : allocatedBuffer(NULL) {}
            void* allocatedBuffer;

            typedef std::map<osg::Transform*, PlayerAnimation::AnimationData> AnimationMap;
            void build(OzzAnimation::AnimationSampler& sampler,
                       std::vector<osg::Transform*> nodes, const AnimationMap& dataMap)
            {
                ozz::animation::Animation& anim = sampler.animation;
                anim.Deallocate(); anim.num_tracks_ = (int)nodes.size();

                // Get max/min time range
                float minTime = 9999.0f, maxTime = 0.0f;
                std::map<osg::Transform*, std::pair<int, int>> trackMap;  // node: index, has tracks
                for (size_t i = 0; i < nodes.size(); ++i)
                    trackMap[nodes[i]] = std::pair<int, int>(i, 0);

                for (AnimationMap::const_iterator itr = dataMap.begin();
                     itr != dataMap.end(); ++itr)
                {
                    const PlayerAnimation::AnimationData& ad = itr->second;
                    if (!ad._positionFrames.empty())
                    {
                        minTime = osg::minimum(ad._positionFrames.front().first, minTime);
                        maxTime = osg::maximum(ad._positionFrames.back().first, maxTime);
                    }
                    if (!ad._rotationFrames.empty())
                    {
                        minTime = osg::minimum(ad._rotationFrames.front().first, minTime);
                        maxTime = osg::maximum(ad._rotationFrames.back().first, maxTime);
                    }
                    if (!ad._scaleFrames.empty())
                    {
                        minTime = osg::minimum(ad._scaleFrames.front().first, minTime);
                        maxTime = osg::maximum(ad._scaleFrames.back().first, maxTime);
                    }

                    //if (ad._positionFrames.empty()) continue;
                    //std::cout << itr->first->getName() << ": " << ad._positionFrames.front().second
                    //          << " -> " << ad._positionFrames.back().second << "\n";
                }
                if (osg::equivalent(maxTime, 0.0f)) return;

                // Get and use every track's default pose as keyframe data
                std::map<osg::Transform*, std::vector<std::pair<float, osg::Vec3>>> defPosTracks, defScaleTracks;
                std::map<osg::Transform*, std::vector<std::pair<float, osg::Vec4>>> defRotTracks;
                std::map<int, osg::Transform*> candicates0;
                for (size_t i = 0; i < nodes.size(); ++i)
                {
                    osg::Transform* t = nodes[i]; candicates0[i] = t;
                    osg::Matrix matrix; osg::Vec3 pos, scale; osg::Quat rot, so;
                    t->computeLocalToWorldMatrix(matrix, NULL);

                    matrix.decompose(pos, rot, scale, so);
                    defPosTracks[t].push_back(std::pair<float, osg::Vec3>(0.0f, pos));
                    defRotTracks[t].push_back(std::pair<float, osg::Vec4>(0.0f, rot.asVec4()));
                    defScaleTracks[t].push_back(std::pair<float, osg::Vec3>(0.0f, scale));
                }

                // Record T/R/S keyframes
                float invD = 1.0f / maxTime; osg::Vec3 defScale(1.0f, 1.0f, 1.0f);
                std::vector<SortingFloat3Key> positions, scales;
                std::vector<SortingQuaternionKey> rotations;
                for (AnimationMap::const_iterator itr = dataMap.begin(); itr != dataMap.end(); ++itr)
                {
                    const PlayerAnimation::AnimationData& ad = itr->second;
                    std::map<std::string, std::string> ipMap = ad._interpolations;
                    std::pair<int, int>& trackData = trackMap[itr->first]; trackData.second += 1;

                    int track = trackData.first; osg::Transform* tr = candicates0[track];
                    sampleData(positions, ipMap["translation"], 0, track, ad._positionFrames, invD,
                               true, defPosTracks[tr].front().second);
                    sampleData(rotations, ipMap["rotation"], 1, track, ad._rotationFrames, invD,
                               true, defRotTracks[tr].front().second);
                    sampleData(scales, ipMap["scale"], 2, track, ad._scaleFrames, invD,
                               true, defScaleTracks[tr].front().second);
                }

                // Fill missed tracks of each frame
                sortData(positions, rotations, scales);
                fillTracksPerFrame(positions, defPosTracks, candicates0);
                fillTracksPerFrame(rotations, defRotTracks, candicates0);
                fillTracksPerFrame(scales, defScaleTracks, candicates0);

                // Record extra tracks for SoaTransform use
                int numSoaTracks = ozz::Align(anim.num_tracks_, 4);
#if false
                osg::Transform* tr0 = nodes[0];  // clear default to generate empty keyframes for extra
                defPosTracks[tr0].clear(); defRotTracks[tr0].clear(); defScaleTracks[tr0].clear();
                for (int j = anim.num_tracks_; j < numSoaTracks; ++j)
                {
                    sampleData(positions, "LINEAR", 0, j, defPosTracks[tr0], 1.0f);
                    sampleData(rotations, "LINEAR", 1, j, defRotTracks[tr0], 1.0f);
                    sampleData(scales, "LINEAR", 2, j, defScaleTracks[tr0], 1.0f, true, defScale);
                }
#else
                sortData(positions, rotations, scales);
                copySamplingData(positions, anim.num_tracks_, numSoaTracks);
                copySamplingData(rotations, anim.num_tracks_, numSoaTracks);
                copySamplingData(scales, anim.num_tracks_, numSoaTracks);
#endif

                // Build time points and controls
                ozz::vector<float> timePoints;
                for (size_t i = 0; i < positions.size(); ++i) timePoints.push_back(positions[i].ratio);
                for (size_t i = 0; i < rotations.size(); ++i) timePoints.push_back(rotations[i].ratio);
                for (size_t i = 0; i < scales.size(); ++i) timePoints.push_back(scales[i].ratio);
                std::sort(timePoints.begin(), timePoints.end());
                timePoints.erase(std::unique(timePoints.begin(), timePoints.end()), timePoints.end());

                float interval = 10.0f, D = maxTime;
                BuilderIFrames t_ss = buildIFrames(ozz::make_span(positions), interval, D, numSoaTracks);
                BuilderIFrames r_ss = buildIFrames(ozz::make_span(rotations), interval, D, numSoaTracks);
                BuilderIFrames s_ss = buildIFrames(ozz::make_span(scales), interval, D, numSoaTracks);

                // Allocate animation buffer
                int pCount = (int)positions.size(), rCount = (int)rotations.size(),
                    sCount = (int)scales.size(), tCount = (int)timePoints.size();
                const size_t sizeOfRatio = (timePoints.size() <= std::numeric_limits<uint8_t>::max())
                                         ? sizeof(uint8_t) : sizeof(uint16_t);
                size_t ctrl_entries = t_ss.entries.size() + r_ss.entries.size() + s_ss.entries.size();
                size_t ctrl_offsets = t_ss.desc.size() + r_ss.desc.size() + s_ss.desc.size();
                size_t bufferSize = pCount * sizeof(Float3Key) + rCount * sizeof(QuaternionKey) +
                                    sCount * sizeof(Float3Key) + tCount * sizeof(float);
                bufferSize += (pCount + rCount + sCount) * (sizeOfRatio + sizeof(uint16_t)) +
                              ctrl_entries * sizeof(ozz::byte) + ctrl_offsets * sizeof(uint32_t);
                allocatedBuffer = memory::default_allocator()->Allocate(bufferSize, alignof(float));

                ozz::span<ozz::byte> buffer = { static_cast<ozz::byte*>(allocatedBuffer), bufferSize };
                anim.duration_ = maxTime;

                // 32b alignment
                anim.timepoints_ = ozz::fill_span<float>(buffer, tCount);
                anim.translations_ctrl_.iframe_desc = ozz::fill_span<uint32_t>(buffer, t_ss.desc.size());
                anim.rotations_ctrl_.iframe_desc = ozz::fill_span<uint32_t>(buffer, r_ss.desc.size());
                anim.scales_ctrl_.iframe_desc = ozz::fill_span<uint32_t>(buffer, s_ss.desc.size());

                // 16b alignment
                anim.translations_ctrl_.previouses = ozz::fill_span<uint16_t>(buffer, pCount);
                anim.rotations_ctrl_.previouses = ozz::fill_span<uint16_t>(buffer, rCount);
                anim.scales_ctrl_.previouses = ozz::fill_span<uint16_t>(buffer, sCount);
                anim.translations_values_ = ozz::fill_span<Float3Key>(buffer, pCount);
                anim.rotations_values_ = ozz::fill_span<QuaternionKey>(buffer, rCount);
                anim.scales_values_ = ozz::fill_span<Float3Key>(buffer, sCount);

                // 16b / 8b alignment
                anim.translations_ctrl_.ratios = ozz::fill_span<ozz::byte>(buffer, pCount * sizeOfRatio);
                anim.rotations_ctrl_.ratios = ozz::fill_span<ozz::byte>(buffer, rCount * sizeOfRatio);
                anim.scales_ctrl_.ratios = ozz::fill_span<ozz::byte>(buffer, sCount * sizeOfRatio);
                anim.translations_ctrl_.iframe_entries = ozz::fill_span<ozz::byte>(buffer, t_ss.entries.size());
                anim.rotations_ctrl_.iframe_entries = ozz::fill_span<ozz::byte>(buffer, r_ss.entries.size());
                anim.scales_ctrl_.iframe_entries = ozz::fill_span<ozz::byte>(buffer, s_ss.entries.size());

                // Sort by ratio/track and fill the animation data
                copyIFrames(t_ss, anim.translations_ctrl_);
                copyIFrames(r_ss, anim.rotations_ctrl_);
                copyIFrames(s_ss, anim.scales_ctrl_);

                sortData(positions, rotations, scales);
                for (int i = 0; i < pCount; ++i) anim.translations_values_[i] = positions[i].key;
                for (int i = 0; i < rCount; ++i) anim.rotations_values_[i] = rotations[i].key;
                for (int i = 0; i < sCount; ++i) anim.scales_values_[i] = scales[i].key;
                if (tCount > 0 && (size_t)tCount <= SIZE_MAX / sizeof(float))
                    memcpy(anim.timepoints_.data(), &timePoints[0], (size_t)tCount * sizeof(float));

                compressData(timePoints, positions, ozz::make_span(anim.translations_values_),
                             numSoaTracks, anim.translations_ctrl_);
                compressData(timePoints, rotations, ozz::make_span(anim.rotations_values_),
                             numSoaTracks, anim.rotations_ctrl_);
                compressData(timePoints, scales, ozz::make_span(anim.scales_values_),
                             numSoaTracks, anim.scales_ctrl_);
            }

        protected:
            struct SortingFloat3Key
            {
                int track; float ratio;
                Float3Key key;
            };

            struct SortingQuaternionKey
            {
                int track; float ratio;
                QuaternionKey key;
            };

            struct BuilderIFrame
            {
                ozz::vector<ozz::byte> entries;
                size_t last;
            };

            struct BuilderIFrames
            {
                ozz::vector<ozz::byte> entries;
                ozz::vector<uint32_t> desc;
                float interval;
            };

            template<typename T1, typename T2>
            void fillTracksPerFrame(T1& inputs, T2& defTrackValues,
                                    const std::map<int, osg::Transform*>& candicates0)
            {
                std::map<int, osg::Transform*> c1;
                float lastR = -1.0f; size_t count = inputs.size();
                for (size_t i = 0; i < count; ++i)
                {
                    float r = inputs[i].ratio; int track = inputs[i].track;
                    if (r != lastR)
                    {
                        for (std::map<int, osg::Transform*>::iterator it = c1.begin(); it != c1.end(); ++it)
                        {
                            osg::Transform* tr = it->second; defTrackValues[tr].front().first = lastR;
                            sampleData(inputs, "LINEAR", 0, it->first, defTrackValues[tr], 1.0f, false);
                        }
                        c1 = candicates0; lastR = r;
                    }

                    std::map<int, osg::Transform*>::iterator itr = c1.find(track);
                    if (itr != c1.end()) c1.erase(itr);
                }

                for (std::map<int, osg::Transform*>::iterator it = c1.begin(); it != c1.end(); ++it)
                {
                    osg::Transform* tr = it->second; defTrackValues[tr].front().first = lastR;
                    sampleData(inputs, "LINEAR", 0, it->first, defTrackValues[tr], 1.0f, false);
                }
            }

            void sortData(std::vector<SortingFloat3Key>& positions,
                          std::vector<SortingQuaternionKey>& rotations,
                          std::vector<SortingFloat3Key>& scales)
            {
                std::sort(positions.begin(), positions.end(), [](SortingFloat3Key& l, SortingFloat3Key& r)
                { if (osg::equivalent(l.ratio, r.ratio)) return l.track < r.track; return l.ratio < r.ratio; });
                std::sort(rotations.begin(), rotations.end(), [](SortingQuaternionKey& l, SortingQuaternionKey& r)
                { if (osg::equivalent(l.ratio, r.ratio)) return l.track < r.track; return l.ratio < r.ratio; });
                std::sort(scales.begin(), scales.end(), [](SortingFloat3Key& l, SortingFloat3Key& r)
                { if (osg::equivalent(l.ratio, r.ratio)) return l.track < r.track; return l.ratio < r.ratio; });
            }

            void sampleData(std::vector<SortingFloat3Key>& values, const std::string& interpo, int type,
                            int track, const std::vector<std::pair<float, osg::Vec3>>& frames, float invD,
                            bool autoFill01 = true, const osg::Vec3& def01 = osg::Vec3())
            {
                if (frames.empty() && autoFill01)
                {
                    SortingFloat3Key k0; k0.ratio = 0.0f; k0.track = track;
                    SortingFloat3Key k1; k1.ratio = 1.0f; k1.track = track;
                    for (int k = 0; k < 3; ++k)
                    {
                        k0.key.values[k] = ozz::math::FloatToHalf(def01[k]);
                        k1.key.values[k] = ozz::math::FloatToHalf(def01[k]);
                    }
                    values.push_back(k0); values.push_back(k1); return;
                }

                // TODO: interpo = LINEAR / STEP / CUBICSPLINE
                for (size_t i = 0; i < frames.size(); ++i)
                {
                    float t = frames[i].first * invD;
                    const osg::Vec3& v = frames[i].second;
                    SortingFloat3Key key; key.ratio = t; key.track = track;
                    key.key.values[0] = ozz::math::FloatToHalf(v[0]);
                    key.key.values[1] = ozz::math::FloatToHalf(v[1]);
                    key.key.values[2] = ozz::math::FloatToHalf(v[2]);

                    if (i == 0 && t > 0.0f && autoFill01)
                    {
                        SortingFloat3Key key0 = key; key0.ratio = 0.0f;
                        values.push_back(key0);
                    }
                    values.push_back(key);
                }

                if (values.back().ratio < 1.0f && autoFill01)
                {
                    SortingFloat3Key key1 = values.back(); key1.ratio = 1.0f;
                    values.push_back(key1);
                }
            }

            void sampleData(std::vector<SortingQuaternionKey>& values, const std::string& interpo, int type,
                            int track, const std::vector<std::pair<float, osg::Vec4>>& frames, float invD,
                            bool autoFill01 = true, const osg::Vec4& def01 = osg::Vec4())
            {
                if (frames.empty() && autoFill01)
                {
                    SortingQuaternionKey k0; k0.ratio = 0.0f; k0.track = track;
                    SortingQuaternionKey k1; k1.ratio = 1.0f; k1.track = track;
                    compressQuat(def01, &(k0.key)); compressQuat(def01, &(k1.key));
                    values.push_back(k0); values.push_back(k1); return;
                }

                // TODO: interpo = LINEAR / STEP / CUBICSPLINE
                for (size_t i = 0; i < frames.size(); ++i)
                {
                    float t = frames[i].first * invD;
                    SortingQuaternionKey key; key.ratio = t; key.track = track;
                    compressQuat(frames[i].second, &(key.key));

                    if (i == 0 && t > 0.0f && autoFill01)
                    {
                        SortingQuaternionKey key0 = key; key0.ratio = 0.0f;
                        values.push_back(key0);
                    }
                    values.push_back(key);
                }

                if (values.back().ratio < 1.0f && autoFill01)
                {
                    SortingQuaternionKey key1 = values.back(); key1.ratio = 1.0f;
                    values.push_back(key1);
                }
            }

            template<typename T>
            void copySamplingData(std::vector<T>& srcData, int numTracks, int numSoaTracks)
            {
                std::vector<T> tempData(srcData); srcData.clear();
                size_t dataSize = tempData.size();
                for (size_t i = 0; i < dataSize; i += numTracks)
                {
                    typename std::vector<T>::iterator it = tempData.begin() + i;
                    if ((i + numTracks) >= dataSize) srcData.insert(srcData.end(), it, tempData.end());
                    else srcData.insert(srcData.end(), it, it + numTracks);

                    T v0 = *it;
                    for (size_t j = numTracks; j < numSoaTracks; ++j)
                        { v0.track = j; srcData.push_back(v0); }
                }
            }

            template<typename T1, typename T2>
            void compressData(const ozz::vector<float>& timePoints, const std::vector<T1>& srcData,
                              const ozz::span<T2>& src, int numSoaTracks, Animation::KeyframesCtrl& dest)
            {
                ozz::vector<const T2*> previouses(numSoaTracks);
                bool use8Bit = (timePoints.size() <= std::numeric_limits<uint8_t>::max());
                for (size_t i = 0; i < src.size(); ++i)
                {
                    const T1& srcKey = srcData[i]; const T2& key = src[i];
                    ozz::vector<float>::const_iterator found =
                        std::lower_bound(timePoints.begin(), timePoints.end(), srcKey.ratio);
                    const uint16_t ratio = static_cast<uint16_t>(found - timePoints.begin());
                    if (use8Bit)
                        ozz::reinterpret_span<uint8_t>(dest.ratios)[i] = static_cast<uint8_t>(ratio);
                    else
                        ozz::reinterpret_span<uint16_t>(dest.ratios)[i] = ratio;

                    const long long diff = (previouses[srcKey.track] != NULL)
                                         ? (&key) - previouses[srcKey.track] : 0;
                    if (diff < ozz::animation::internal::kMaxPreviousOffset)
                        dest.previouses[i] = static_cast<uint16_t>(diff);
                    previouses[srcKey.track] = &key;
                }
            }

            template<typename T>
            BuilderIFrame buildIFrame(const ozz::span<T>& src, float ratio, int numSoaTracks)
            {
                BuilderIFrame iframe;
                ozz::vector<uint32_t> entries(numSoaTracks);
                for (size_t i = 0, end = src.size(); i < end && src[i].ratio <= ratio; ++i)
                {   // Stores the last key found for this track.
                    entries[src[i].track] = static_cast<uint32_t>(i); iframe.last = i;
                }

                // Entries is a multiple of 4 (number os soa tracks).
                iframe.entries.resize(ozz::ComputeGV4WorstBufferSize(ozz::make_span(entries)));
                ozz::span<ozz::byte> remain =
                    ozz::EncodeGV4Stream(ozz::make_span(entries), ozz::make_span(iframe.entries));
                iframe.entries.resize(iframe.entries.size() - remain.size_bytes()); return iframe;
            }

            template<typename T>
            BuilderIFrames buildIFrames(const ozz::span<T>& src, float interval,
                                        float duration, int numSoaTracks)
            {
                BuilderIFrames iframes; iframes.interval = 1.0f;
                size_t iframes_divs = static_cast<size_t>(math::Max(1.0f, duration / interval));
                for (size_t i = 0; i < iframes_divs; ++i)
                {
                    float time = (float)(i + 1) / iframes_divs;
                    BuilderIFrame f = buildIFrame(src, time, numSoaTracks);

                    // Don't need to add an iframe for the first set of keyframes
                    if (f.last <= numSoaTracks * 2 - 1) continue;
                    if (!iframes.desc.empty() && f.last <= iframes.desc.back()) continue;

                    // Pushes offset and compressed data
                    iframes.desc.push_back(static_cast<uint32_t>(iframes.entries.size()));
                    iframes.desc.push_back(static_cast<uint32_t>(f.last));
                    iframes.entries.insert(iframes.entries.end(), f.entries.begin(), f.entries.end());
                }

                // Computes actual interval (duration ratio) between iframes
                if (!iframes.entries.empty()) iframes.interval = (2.0f / iframes.desc.size());
                return iframes;
            }

            void copyIFrames(const BuilderIFrames& src, Animation::KeyframesCtrl& dest)
            {
                std::copy(src.entries.begin(), src.entries.end(), dest.iframe_entries.begin());
                std::copy(src.desc.begin(), src.desc.end(), dest.iframe_desc.begin());
                dest.iframe_interval = src.interval;
            }

            static void compressQuat(const osg::Vec4& src, QuaternionKey* dest)
            {
                // Finds the largest quaternion component.
                const float quat[4] = { src.x(), src.y(), src.z(), src.w() };
                const long long largest = std::max_element(quat, quat + 4, lessAbs) - quat;

                // Quantize the 3 smallest components on 16 bits signed integers.
                // Quantize the 3 smallest components on x bits signed integers.
                const float kScale = QuaternionKey::kfScale / math::kSqrt2, kOffset = -math::kSqrt2_2;
                const int kMapping[4][3] = { {1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2} };
                const int* map = kMapping[largest];
                const int cpnt[3] = {
                    math::Min(static_cast<int>((quat[map[0]] - kOffset) * kScale + 0.5f),
                              internal::QuaternionKey::kiScale),
                    math::Min(static_cast<int>((quat[map[1]] - kOffset) * kScale + 0.5f),
                              internal::QuaternionKey::kiScale),
                    math::Min(static_cast<int>((quat[map[2]] - kOffset) * kScale + 0.5f),
                              internal::QuaternionKey::kiScale) };
                ozz::animation::internal::pack(static_cast<int>(largest), quat[largest] < 0.f, cpnt, dest);
            }

            static bool lessAbs(float _left, float _right)
            { return std::abs(_left) < std::abs(_right); }
        };
    }
}

namespace
{
    static std::string printPlayerData(OzzAnimation* ozz, bool withJointWeights = false)
    {
        ozz::span<const char* const> names = ozz->_skeleton.joint_names();
        ozz::span<const int16_t> parents = ozz->_skeleton.joint_parents();
        ozz::span<const ozz::math::SoaTransform> restPoses = ozz->_skeleton.joint_rest_poses();
        std::stringstream out; out << "Total bones: " << names.size() << std::endl;
        for (size_t i = 0; i < names.size(); ++i)
        {
            int16_t pid = parents[i], soaID = i % 4; const ozz::math::SoaTransform soaT = restPoses[i / 4];
            out << "B" << i << ": " << names[i] << ", Parent = " << (pid >= 0 ? names[pid] : "(null)") << std::endl;

            osg::Vec3 pos, scale; osg::Quat rot;
            switch (soaID)
            {
            case 0:
                scale[0] = ozz::math::GetX(soaT.scale.x); scale[1] = ozz::math::GetX(soaT.scale.y);
                scale[2] = ozz::math::GetX(soaT.scale.z);
                rot[0] = ozz::math::GetX(soaT.rotation.x); rot[1] = ozz::math::GetX(soaT.rotation.y);
                rot[2] = ozz::math::GetX(soaT.rotation.z); rot[3] = ozz::math::GetX(soaT.rotation.w);
                pos[0] = ozz::math::GetX(soaT.translation.x); pos[1] = ozz::math::GetX(soaT.translation.y);
                pos[2] = ozz::math::GetX(soaT.translation.z); break;
            case 1:
                scale[0] = ozz::math::GetY(soaT.scale.x); scale[1] = ozz::math::GetY(soaT.scale.y);
                scale[2] = ozz::math::GetY(soaT.scale.z);
                rot[0] = ozz::math::GetY(soaT.rotation.x); rot[1] = ozz::math::GetY(soaT.rotation.y);
                rot[2] = ozz::math::GetY(soaT.rotation.z); rot[3] = ozz::math::GetY(soaT.rotation.w);
                pos[0] = ozz::math::GetY(soaT.translation.x); pos[1] = ozz::math::GetY(soaT.translation.y);
                pos[2] = ozz::math::GetY(soaT.translation.z); break;
            case 2:
                scale[0] = ozz::math::GetZ(soaT.scale.x); scale[1] = ozz::math::GetZ(soaT.scale.y);
                scale[2] = ozz::math::GetZ(soaT.scale.z);
                rot[0] = ozz::math::GetZ(soaT.rotation.x); rot[1] = ozz::math::GetZ(soaT.rotation.y);
                rot[2] = ozz::math::GetZ(soaT.rotation.z); rot[3] = ozz::math::GetZ(soaT.rotation.w);
                pos[0] = ozz::math::GetZ(soaT.translation.x); pos[1] = ozz::math::GetZ(soaT.translation.y);
                pos[2] = ozz::math::GetZ(soaT.translation.z); break;
            case 3:
                scale[0] = ozz::math::GetW(soaT.scale.x); scale[1] = ozz::math::GetW(soaT.scale.y);
                scale[2] = ozz::math::GetW(soaT.scale.z);
                rot[0] = ozz::math::GetW(soaT.rotation.x); rot[1] = ozz::math::GetW(soaT.rotation.y);
                rot[2] = ozz::math::GetW(soaT.rotation.z); rot[3] = ozz::math::GetW(soaT.rotation.w);
                pos[0] = ozz::math::GetW(soaT.translation.x); pos[1] = ozz::math::GetW(soaT.translation.y);
                pos[2] = ozz::math::GetW(soaT.translation.z); break;
            }
            out.setf(std::ios_base::fixed);
            out << std::setprecision(2) << "  Position = " << pos << ", Scale = " << scale
                << ", Rotation = " << computeHPRFromQuat(rot) << std::endl;
        }
        
        out << std::endl;
        for (size_t i = 0; i < ozz->_meshes.size(); ++i)
        {
            OzzMesh& mesh = ozz->_meshes[i];
            out << "Mesh-" << i << ": Parts = " << mesh.parts.size() << "; Bones: " << mesh.num_joints()
                << "; RemappedBones: " << mesh.joint_remaps.size() << std::endl;
            for (size_t j = 0; j < mesh.parts.size(); ++j)
            {
                OzzMesh::Part& part = mesh.parts[j];
                int numVertices = part.vertex_count(), numInfluences = part.influences_count();
                ozz::vector<uint16_t> jointIds = part.joint_indices;
                ozz::vector<float> jointWeights = part.joint_weights;
                out << "  Part-" << j << ": Vertices = " << numVertices << ", Influences = "
                    << numInfluences << ", JointIDs = " << jointIds.size()
                    << ", Weights = " << jointWeights.size() << std::endl;

                if (!withJointWeights) continue;
                for (size_t n = 0; n < numVertices; ++n)
                {
                    size_t n0 = n * 4, n1 = n * 3; out << "    Vec-" << n << ": ";
                    float w = jointWeights[n1] + jointWeights[n1 + 1] + jointWeights[n1 + 2];
                    for (int k = 0; k < 4; ++k)
                    {
                        float weight = jointWeights[n1 + k];
                        if (k == 3) weight = 1.0f - w; if (weight == 0.0f) continue;
                        out << "B" << jointIds[n0 + k] << "/" << jointWeights[n1 + k] << "; ";
                    }
                    out << std::endl;
                }
            }
            out << std::endl;
        }
        return out.str();
    }

    static float keyRatio(const ozz::span<const float>& timepoints,
                        const ozz::span<const ozz::byte>& ratios, size_t at)
    {
        if (timepoints.size() <= std::numeric_limits<uint8_t>::max())
            return timepoints[ozz::reinterpret_span<const uint8_t>(ratios)[at]];
        else
            return timepoints[ozz::reinterpret_span<const uint16_t>(ratios)[at]];
    }

    static void printAnimationData(OzzAnimation* ozz, const std::string& key)
    {
        ozz::span<const char* const> names = ozz->_skeleton.joint_names();
        OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
        ozz::animation::Animation& animation = sampler.animation;

        ozz::span<const float> timeList = sampler.animation.timepoints();
        ozz::span<const Float3Key> posList = sampler.animation.translations_values();
        ozz::span<const QuaternionKey> rotList = sampler.animation.rotations_values();
        ozz::span<const Float3Key> scaleList = sampler.animation.scales_values();
        std::cout << "Anim " << key << ": Duration = " << sampler.animation.duration()
                  << ", Frames = " << timeList.size() << ", T/R/S = " << posList.size()
                  << "/" << rotList.size() << "/" << scaleList.size() << std::endl;

        const ozz::span<const ozz::byte>& ratiosT = sampler.animation.translations_ctrl().ratios;
        std::cout << "  Translation Ratios (" << ratiosT.size() << "): " << keyRatio(timeList, ratiosT, 0)
                  << " --> " << keyRatio(timeList, ratiosT, ratiosT.size() - 1) << ", IFrame Entries = "
                  << sampler.animation.translations_ctrl().iframe_entries.size() << ", Desc = "
                  << sampler.animation.translations_ctrl().iframe_desc.size() << std::endl;
        const ozz::span<const ozz::byte>& ratiosR = sampler.animation.rotations_ctrl().ratios;
        std::cout << "  Rotation Ratios (" << ratiosR.size() << "): " << keyRatio(timeList, ratiosR, 0)
                  << " --> " << keyRatio(timeList, ratiosR, ratiosR.size() - 1) << ", IFrame Entries = "
                  << sampler.animation.rotations_ctrl().iframe_entries.size()  << ", Desc = "
                  << sampler.animation.rotations_ctrl().iframe_desc.size() << std::endl;
        const ozz::span<const ozz::byte>& ratiosS = sampler.animation.scales_ctrl().ratios;
        std::cout << "  Scale Ratios (" << ratiosS.size() << "): " << keyRatio(timeList, ratiosS, 0)
                  << " --> " << keyRatio(timeList, ratiosS, ratiosS.size() - 1) << ", IFrame Entries = "
                  << sampler.animation.scales_ctrl().iframe_entries.size()  << ", Desc = "
                  << sampler.animation.scales_ctrl().iframe_desc.size() << std::endl;

        for (size_t i = 0; i < names.size(); ++i)
        {
            int tFrames = ozz::animation::CountTranslationKeyframes(sampler.animation, i);
            int rFrames = ozz::animation::CountRotationKeyframes(sampler.animation, i);
            int sFrames = ozz::animation::CountScaleKeyframes(sampler.animation, i);
            std::cout << "  T-" << i << " (" << names[i] << "): TranslationKey = " << tFrames
                      << ", RotationKey = " << rFrames << ", ScaleKey = " << sFrames << std::endl;
        }
        std::cout << std::endl;
    }
}

osg::AnimationPath* PlayerAnimation::AnimationData::toAnimationPath() const
{
    osg::AnimationPath::TimeControlPointMap ctrlMap; std::map<float, int> timePoints;
    for (size_t i = 0; i < _scaleFrames.size(); ++i)
    {
        const std::pair<float, osg::Vec3>& v = _scaleFrames[i];
        ctrlMap[v.first].setScale(v.second); timePoints[v.first] += 1;
    }
    for (size_t i = 0; i < _positionFrames.size(); ++i)
    {
        const std::pair<float, osg::Vec3>& v = _positionFrames[i];
        ctrlMap[v.first].setPosition(v.second); timePoints[v.first] += 2;
    }
    for (size_t i = 0; i < _rotationFrames.size(); ++i)
    {
        const std::pair<float, osg::Vec4>& v = _rotationFrames[i];
        ctrlMap[v.first].setRotation(osg::Quat(v.second)); timePoints[v.first] += 4;
    }

    for (std::map<float, int>::iterator itr = timePoints.begin(); itr != timePoints.end(); ++itr)
    {
        if (!(itr->second & 1)) ctrlMap[itr->first].setScale(
            _scaleFrames.empty() ? osg::Vec3(1.0f, 1.0f, 1.0f) : _scaleFrames.front().second);
        if (!(itr->second & 2)) ctrlMap[itr->first].setPosition(
            _positionFrames.empty() ? osg::Vec3() : _positionFrames.front().second);
        if (!(itr->second & 4)) ctrlMap[itr->first].setRotation(
            _rotationFrames.empty() ? osg::Quat() : osg::Quat(_rotationFrames.front().second));
    }

    osg::AnimationPath* path = new osg::AnimationPath;
    path->setLoopMode(osg::AnimationPath::LOOP);
    path->setTimeControlPointMap(ctrlMap); return path;
}

void PlayerAnimation::AnimationData::fromAnimationPath(const osg::AnimationPath* path)
{
    const osg::AnimationPath::TimeControlPointMap& ctrlMap = path->getTimeControlPointMap();
    for (std::map<double, osg::AnimationPath::ControlPoint>::const_iterator itr2 = ctrlMap.begin();
        itr2 != ctrlMap.end(); ++itr2)
    {
        const osg::AnimationPath::ControlPoint& cp = itr2->second; float t = itr2->first;
        _scaleFrames.push_back(std::pair<float, osg::Vec3>(t, cp.getScale()));
        _positionFrames.push_back(std::pair<float, osg::Vec3>(t, cp.getPosition()));
        _rotationFrames.push_back(std::pair<float, osg::Vec4>(t, cp.getRotation().asVec4()));
    }
}

PlayerAnimation::PlayerAnimation()
{
    _internal = new OzzAnimation; _animated = true; _externalDriven = false;
    _drawSkeleton = true; _drawSkinning = true; _restPose = false;
    _blendingThreshold = ozz::animation::BlendingJob().threshold;
}

PlayerAnimation::~PlayerAnimation()
{
}

PlayerAnimation::PlayerAnimation(const PlayerAnimation& copy, const osg::CopyOp& copyop)
:   osg::NodeCallback(copy, copyop), _blendshapes(copy._blendshapes),
    _meshStateSetList(copy._meshStateSetList), _modelRoot(copy._modelRoot),
    _skeletonRoot(copy._skeletonRoot), _blendingThreshold(copy._blendingThreshold),
    _animated(copy._animated), _drawSkeleton(copy._drawSkeleton),
    _drawSkinning(copy._drawSkinning), _restPose(copy._restPose),
    _externalDriven(copy._externalDriven), _jointBindings(copy._jointBindings),
    _meshBindings(copy._meshBindings), _animationFiles(copy._animationFiles),
    _skeletonFile(copy._skeletonFile), _meshFile(copy._meshFile)
{
    _internal = new OzzAnimation;
}

void PlayerAnimation::setSkinningValidator(ValidateSkinningFunc func)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz) ozz->_validator = func;
}

namespace
{
    static void decomposeSoaTransform(const ozz::math::SoaTransform& soaT, int id,
                                      osg::Vec3& pos, osg::Quat& rot, osg::Vec3& scale)
    {
        switch (id)
        {
        case 0:
            scale[0] = ozz::math::GetX(soaT.scale.x); scale[1] = ozz::math::GetX(soaT.scale.y);
            scale[2] = ozz::math::GetX(soaT.scale.z);
            rot[0] = ozz::math::GetX(soaT.rotation.x); rot[1] = ozz::math::GetX(soaT.rotation.y);
            rot[2] = ozz::math::GetX(soaT.rotation.z); rot[3] = ozz::math::GetX(soaT.rotation.w);
            pos[0] = ozz::math::GetX(soaT.translation.x); pos[1] = ozz::math::GetX(soaT.translation.y);
            pos[2] = ozz::math::GetX(soaT.translation.z); break;
        case 1:
            scale[0] = ozz::math::GetY(soaT.scale.x); scale[1] = ozz::math::GetY(soaT.scale.y);
            scale[2] = ozz::math::GetY(soaT.scale.z);
            rot[0] = ozz::math::GetY(soaT.rotation.x); rot[1] = ozz::math::GetY(soaT.rotation.y);
            rot[2] = ozz::math::GetY(soaT.rotation.z); rot[3] = ozz::math::GetY(soaT.rotation.w);
            pos[0] = ozz::math::GetY(soaT.translation.x); pos[1] = ozz::math::GetY(soaT.translation.y);
            pos[2] = ozz::math::GetY(soaT.translation.z); break;
        case 2:
            scale[0] = ozz::math::GetZ(soaT.scale.x); scale[1] = ozz::math::GetZ(soaT.scale.y);
            scale[2] = ozz::math::GetZ(soaT.scale.z);
            rot[0] = ozz::math::GetZ(soaT.rotation.x); rot[1] = ozz::math::GetZ(soaT.rotation.y);
            rot[2] = ozz::math::GetZ(soaT.rotation.z); rot[3] = ozz::math::GetZ(soaT.rotation.w);
            pos[0] = ozz::math::GetZ(soaT.translation.x); pos[1] = ozz::math::GetZ(soaT.translation.y);
            pos[2] = ozz::math::GetZ(soaT.translation.z); break;
        default:
            scale[0] = ozz::math::GetW(soaT.scale.x); scale[1] = ozz::math::GetW(soaT.scale.y);
            scale[2] = ozz::math::GetW(soaT.scale.z);
            rot[0] = ozz::math::GetW(soaT.rotation.x); rot[1] = ozz::math::GetW(soaT.rotation.y);
            rot[2] = ozz::math::GetW(soaT.rotation.z); rot[3] = ozz::math::GetW(soaT.rotation.w);
            pos[0] = ozz::math::GetW(soaT.translation.x); pos[1] = ozz::math::GetW(soaT.translation.y);
            pos[2] = ozz::math::GetW(soaT.translation.z); break;
        }
    }
}

bool PlayerAnimation::exportJointBindings()
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz == NULL) return false;

    ozz::span<const char* const> names = ozz->_skeleton.joint_names();
    ozz::span<const int16_t> parents = ozz->_skeleton.joint_parents();
    ozz::span<const ozz::math::SoaTransform> restPoses = ozz->_skeleton.joint_rest_poses();
    if (names.empty() || parents.size() != names.size() || restPoses.empty()) return false;

    _jointBindings.resize(names.size());
    for (size_t i = 0; i < names.size(); ++i)
    {
        JointBinding& j = _jointBindings[i];
        j.name = names[i] ? names[i] : "";
        j.parent = parents[i];
        decomposeSoaTransform(restPoses[i / 4], (int)(i % 4), j.translation, j.rotation, j.scale);
    }
    return true;
}

bool PlayerAnimation::exportMeshBindings()
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz == NULL) return false;

    const int numJoints = ozz->_skeleton.num_joints();
    _meshBindings.clear(); _meshBindings.resize(ozz->_meshes.size());
    for (size_t mi = 0; mi < ozz->_meshes.size(); ++mi)
    {
        const OzzMesh& mesh = ozz->_meshes[mi];
        MeshBinding& dst = _meshBindings[mi];
        dst.name = (mi < ozz->_meshNames.size()) ? ozz->_meshNames[mi] : std::string();
        dst.stateSet = (mi < _meshStateSetList.size()) ? _meshStateSetList[mi] : NULL;
        dst.blendshape = (mi < _blendshapes.size()) ? _blendshapes[mi] : NULL;

        size_t totalVertices = 0;
        bool hasNormals = true, hasTangents = true, hasUVs = true, hasColors = true;
        for (size_t p = 0; p < mesh.parts.size(); ++p)
        {
            const OzzMesh::Part& part = mesh.parts[p];
            size_t vCount = part.vertex_count(); totalVertices += vCount;
            if (part.normals.size() != vCount * 3) hasNormals = false;
            if (part.tangents.size() != vCount * 4) hasTangents = false;
            if (part.uvs.size() != vCount * 2) hasUVs = false;
            if (part.colors.size() != vCount * 4) hasColors = false;
        }
        if (totalVertices < 1) continue;

        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(totalVertices);
        osg::ref_ptr<osg::Vec3Array> normals = hasNormals ? new osg::Vec3Array(totalVertices) : NULL;
        osg::ref_ptr<osg::Vec4Array> tangents = hasTangents ? new osg::Vec4Array(totalVertices) : NULL;
        osg::ref_ptr<osg::Vec2Array> uvs = hasUVs ? new osg::Vec2Array(totalVertices) : NULL;
        osg::ref_ptr<osg::Vec4ubArray> colors = hasColors ? new osg::Vec4ubArray(totalVertices) : NULL;
        dst.jointIndices.assign(totalVertices * 4, 0);
        dst.jointWeights.assign(totalVertices * 4, 0.0f);

        size_t vOffset = 0;
        for (size_t p = 0; p < mesh.parts.size(); ++p)
        {
            const OzzMesh::Part& part = mesh.parts[p];
            size_t vCount = part.vertex_count(); int inf = part.influences_count();
            for (size_t v = 0; v < vCount; ++v)
            {
                size_t vi = vOffset + v;
                (*positions)[vi] = osg::Vec3(part.positions[v * 3 + 0],
                                             part.positions[v * 3 + 1], part.positions[v * 3 + 2]);
                if (normals.valid())
                    (*normals)[vi] = osg::Vec3(part.normals[v * 3 + 0],
                                               part.normals[v * 3 + 1], part.normals[v * 3 + 2]);
                if (tangents.valid())
                    (*tangents)[vi] = osg::Vec4(part.tangents[v * 4 + 0], part.tangents[v * 4 + 1],
                                                part.tangents[v * 4 + 2], part.tangents[v * 4 + 3]);
                if (uvs.valid())
                    (*uvs)[vi] = osg::Vec2(part.uvs[v * 2 + 0], part.uvs[v * 2 + 1]);
                if (colors.valid())
                    (*colors)[vi] = osg::Vec4ub(part.colors[v * 4 + 0], part.colors[v * 4 + 1],
                                                part.colors[v * 4 + 2], part.colors[v * 4 + 3]);

                uint16_t ids[4] = { 0, 0, 0, 0 }; float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                if (inf > 0)
                {
                    int count = osg::minimum(inf, 4);
                    float sum = 0.0f;
                    for (int k = 0; k < count; ++k)
                    {
                        ids[k] = part.joint_indices[v * inf + k];
                        if (k < inf - 1) { weights[k] = part.joint_weights[v * (inf - 1) + k]; sum += weights[k]; }
                    }
                    weights[count - 1] = osg::maximum(0.0f, 1.0f - sum);
                }
                else
                    weights[0] = 1.0f;
                for (int k = 0; k < 4; ++k)
                {
                    dst.jointIndices[vi * 4 + k] = ids[k];
                    dst.jointWeights[vi * 4 + k] = weights[k];
                }
            }
            vOffset += vCount;
        }
        dst.positions = positions; dst.normals = normals;
        dst.tangents = tangents; dst.uvs = uvs; dst.colors = colors;

        if (!mesh.triangle_indices.empty())
        {
            dst.indices = new osg::DrawElementsUInt(GL_TRIANGLES);
            dst.indices->reserve(mesh.triangle_indices.size());
            for (size_t i = 0; i < mesh.triangle_indices.size(); ++i)
                dst.indices->push_back((unsigned int)mesh.triangle_indices[i]);
        }

        dst.inverseBindPoses.assign(numJoints, osg::Matrix::identity());
        for (size_t k = 0; k < mesh.joint_remaps.size() && k < mesh.inverse_bind_poses.size(); ++k)
        {
            int joint = mesh.joint_remaps[k];
            if (joint >= 0 && joint < numJoints)
                dst.inverseBindPoses[joint] = OzzAnimation::convertMatrix(mesh.inverse_bind_poses[k]);
        }
    }
    return true;
}

bool PlayerAnimation::initialize(const std::vector<JointBinding>& joints,
                                 const std::vector<MeshBinding>& meshes)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (joints.empty())
    {
        OSG_WARN << "[PlayerAnimation] Empty skeleton binding." << std::endl; return false;
    }
    _jointBindings = joints; _meshBindings = meshes;
    _skeletonFile.clear(); _meshFile.clear();

    // Build skeleton directly from the index based binding
    ozz::animation::CreateSkeletonVisitor csv;
    csv.build(ozz->_skeleton, joints);
    const int numJoints = ozz->_skeleton.num_joints();
    if (numJoints < 1) return false;

    ozz->_meshes.clear(); ozz->_meshNames.clear();
    _meshStateSetList.clear(); _blendshapes.clear();
    for (size_t m = 0; m < meshes.size(); ++m)
    {
        const MeshBinding& src = meshes[m];
        osg::Vec3Array* va = src.positions.get();
        size_t vCount = va ? va->size() : 0;
        if (vCount < 1)
        {
            OSG_WARN << "[PlayerAnimation] Skip empty mesh binding: " << src.name << std::endl;
            continue;
        }
        else if (vCount > 65535)  // OZZ mesh indices are 16-bit
        {
            OSG_WARN << "[PlayerAnimation] Mesh " << src.name << " has " << vCount
                     << " vertices which exceeds the OZZ 16-bit index limit." << std::endl;
            continue;
        }

        OzzMesh mesh; OzzMesh::Part part;
        part.positions.resize(vCount * 3);
        memcpy(&part.positions[0], &(*va)[0], vCount * sizeof(float) * 3);
        if (src.normals.valid() && src.normals->size() == vCount)
        {
            part.normals.resize(vCount * 3);
            memcpy(&part.normals[0], &(*src.normals)[0], vCount * sizeof(float) * 3);
        }
        if (src.tangents.valid() && src.tangents->size() == vCount)
        {
            part.tangents.resize(vCount * 4);
            memcpy(&part.tangents[0], &(*src.tangents)[0], vCount * sizeof(float) * 4);
        }
        if (src.uvs.valid() && src.uvs->size() == vCount)
        {
            part.uvs.resize(vCount * 2);
            memcpy(&part.uvs[0], &(*src.uvs)[0], vCount * sizeof(float) * 2);
        }
        if (src.colors.valid() && src.colors->size() == vCount)
        {
            part.colors.resize(vCount * 4);
            memcpy(&part.colors[0], &(*src.colors)[0], vCount * sizeof(uint8_t) * 4);
        }

        // 4 joint indices and 3 weights per vertex; the 4th weight is the remainder for OZZ
        const bool validWeights = (src.jointIndices.size() == vCount * 4 &&
                                   src.jointWeights.size() == vCount * 4);
        if (!validWeights)
        {
            OSG_WARN << "[PlayerAnimation] Mesh " << src.name << " has invalid joint weights, "
                     << "all vertices will be bound to the root joint." << std::endl;
        }

        part.joint_indices.resize(vCount * 4);
        part.joint_weights.resize(vCount * 3);
        for (size_t v = 0; v < vCount; ++v)
        {
            uint16_t ids[4] = { 0, 0, 0, 0 }; float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            if (validWeights)
            {
                for (int k = 0; k < 4; ++k)
                {
                    int id = (int)src.jointIndices[v * 4 + k];
                    ids[k] = (id >= 0 && id < numJoints) ? (uint16_t)id : (uint16_t)0;
                    w[k] = osg::maximum(src.jointWeights[v * 4 + k], 0.0f);
                }
            }

            float sum = w[0] + w[1] + w[2] + w[3];
            if (sum <= 0.0f) { ids[0] = 0; w[0] = 1.0f; sum = 1.0f; }
            for (int k = 0; k < 4; ++k)
            {
                part.joint_indices[v * 4 + k] = ids[k];
                if (k < 3) part.joint_weights[v * 3 + k] = w[k] / sum;
            }
        }

        if (src.indices.valid())
        {
            size_t count = src.indices->size();
            mesh.triangle_indices.resize(count);
            for (size_t i = 0; i < count; ++i)
                mesh.triangle_indices[i] = (uint16_t)((*src.indices)[i]);
        }

        // OZZ requires sorted joint remaps and matching inverse bind poses
        mesh.joint_remaps.resize(numJoints);
        mesh.inverse_bind_poses.resize(numJoints, ozz::math::Float4x4::identity());
        for (int j = 0; j < numJoints; ++j)
        {
            mesh.joint_remaps[j] = (uint16_t)j;
            if (j >= (int)src.inverseBindPoses.size()) continue;

            const osg::Matrix& mat = src.inverseBindPoses[j];
            ozz::math::Float4x4& pose = mesh.inverse_bind_poses[j];
            pose.cols[0] = ozz::math::simd_float4::Load(
                mat(0, 0), mat(0, 1), mat(0, 2), mat(0, 3));
            pose.cols[1] = ozz::math::simd_float4::Load(
                mat(1, 0), mat(1, 1), mat(1, 2), mat(1, 3));
            pose.cols[2] = ozz::math::simd_float4::Load(
                mat(2, 0), mat(2, 1), mat(2, 2), mat(2, 3));
            pose.cols[3] = ozz::math::simd_float4::Load(
                mat(3, 0), mat(3, 1), mat(3, 2), mat(3, 3));
        }

        mesh.parts.push_back(part);
        ozz->_meshes.push_back(mesh);
        ozz->_meshNames.push_back(src.name.empty() ? std::string("Mesh") : src.name);
        _meshStateSetList.push_back(src.stateSet);
        _blendshapes.push_back(src.blendshape);
    }
    return initializeInternal();
}

bool PlayerAnimation::initialize(osg::Node& skeletonRoot, osg::Node& meshRoot,
                                 const std::map<osg::Geometry*, GeometryJointData>& jointDataMap)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());

    // Load skeleton data from 'skeletonRoot'
    ozz::animation::CreateSkeletonVisitor csv;
    skeletonRoot.accept(csv); csv.build(ozz->_skeleton);
    if (!csv.getSkeletonNodes().empty()) _skeletonRoot = csv.getSkeletonNodes().front();
    else { OSG_WARN << "[PlayerAnimation] No skeleton node found." << std::endl; return false; }

    // Load mesh data from 'meshRoot' and 'jointDataMap'
    ozz::animation::CreateMeshVisitor cmv(csv.getSkeletonNodes(), jointDataMap);
    meshRoot.accept(cmv); ozz->_meshes = cmv.getMeshes(ozz->_meshNames);
    _meshStateSetList = cmv.getStateSets(); _blendshapes = cmv.getBS();
    _skeletonFile.clear(); _meshFile.clear();
    exportJointBindings(); exportMeshBindings();
    return initializeInternal();
}

bool PlayerAnimation::initialize(const std::vector<osg::Transform*>& nodes,
                                 const std::vector<osg::Geometry*>& meshList,
                                 const std::map<osg::Geometry*, GeometryJointData>& jointDataMap)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    ozz::animation::CreateSkeletonVisitor csv;
    csv.initialize(nodes); csv.build(ozz->_skeleton);
    if (!csv.getSkeletonNodes().empty()) _skeletonRoot = csv.getSkeletonNodes().front();
    else { OSG_WARN << "[PlayerAnimation] No skeleton node given." << std::endl; return false; }

    // TODO: girl.fbx have multiple players? It will call initialize() multiple times!
    ozz::animation::CreateMeshVisitor cmv(csv.getSkeletonNodes(), jointDataMap);
    cmv.initialize(meshList); ozz->_meshes = cmv.getMeshes(ozz->_meshNames);
    _meshStateSetList = cmv.getStateSets(); _blendshapes = cmv.getBS();
    _skeletonFile.clear(); _meshFile.clear();
    exportJointBindings(); exportMeshBindings();
    return initializeInternal();
}

bool PlayerAnimation::initialize(const std::string& skeleton, const std::string& mesh)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (!ozz->loadSkeleton(skeleton.c_str(), &(ozz->_skeleton))) return false;
    if (!ozz->loadMesh(mesh.c_str(), &(ozz->_meshes))) return false;
    _skeletonFile = skeleton; _meshFile = mesh;
    _jointBindings.clear(); _meshBindings.clear();
    return initializeInternal();
}

std::string PlayerAnimation::report(bool withJointWeights) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    return printPlayerData(ozz, withJointWeights);
}

bool PlayerAnimation::loadAnimation(const std::string& key, const std::string& animation)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    if (!ozz->loadAnimation(animation.c_str(), &(sampler.animation))) return false;
    _animationFiles[key] = animation; _animationDataList.erase(key);
    return loadAnimationInternal(key);
}

bool PlayerAnimation::loadAnimation(const std::string& key, const std::vector<osg::Transform*>& nodes,
                                    const std::map<osg::Transform*, AnimationData>& animDataMap)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];

    ozz::animation::AnimationConverter ac;
    ac.build(sampler, nodes, animDataMap); sampler.looping = true;
    if (sampler.buffer != NULL)  // release the buffer of a previous animation with same key
        ozz::memory::default_allocator()->Deallocate(sampler.buffer);
    sampler.buffer = ac.allocatedBuffer;  // ownership moved to the sampler

    // Keep the track data indexed by joint, so the animation can be saved without scene nodes
    std::vector<AnimationData> dataList(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        std::map<osg::Transform*, AnimationData>::const_iterator itr = animDataMap.find(nodes[i]);
        if (itr != animDataMap.end()) dataList[i] = itr->second;
    }
    _animationDataList[key] = dataList; _animationFiles.erase(key);
    return loadAnimationInternal(key);
}

bool PlayerAnimation::loadAnimation(const std::string& key, const std::vector<AnimationData>& dataList)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz == NULL || _jointBindings.empty()) return false;

    // Rebuild the animation from joint indices: temporary transforms only provide rest poses
    std::vector<osg::ref_ptr<osg::MatrixTransform>> holders;
    std::vector<osg::Transform*> nodes;
    std::map<osg::Transform*, AnimationData> animDataMap;
    holders.reserve(_jointBindings.size()); nodes.reserve(_jointBindings.size());
    for (size_t i = 0; i < _jointBindings.size(); ++i)
    {
        osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform(_jointBindings[i].toMatrix());
        holders.push_back(node); nodes.push_back(node.get());
        if (i < dataList.size()) animDataMap[node.get()] = dataList[i];
    }

    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    ozz::animation::AnimationConverter ac;
    ac.build(sampler, nodes, animDataMap); sampler.looping = true;
    if (sampler.buffer != NULL)
        ozz::memory::default_allocator()->Deallocate(sampler.buffer);
    sampler.buffer = ac.allocatedBuffer;
    _animationDataList[key] = dataList; _animationFiles.erase(key);
    return loadAnimationInternal(key);
}

bool PlayerAnimation::loadAnimation(const std::string& key, const std::vector<osg::Transform*>& nodes,
                                    const std::map<osg::Transform*, osg::ref_ptr<osg::AnimationPath>>& pathMap)
{
    std::map<osg::Transform*, AnimationData> animDataMap;
    for (std::map<osg::Transform*, osg::ref_ptr<osg::AnimationPath>>::const_iterator itr = pathMap.begin();
         itr != pathMap.end(); ++itr)
    {
        const osg::Transform* trans = itr->first; const osg::AnimationPath* path = itr->second.get();
        if (std::find(nodes.begin(), nodes.end(), trans) == nodes.end()) continue;

        AnimationData animData; animData.fromAnimationPath(path);
        animDataMap[itr->first] = animData;
    }
    return loadAnimation(key, nodes, animDataMap);
}

void PlayerAnimation::unloadAnimation(const std::string& key)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    std::map<std::string, OzzAnimation::AnimationSampler>::iterator itr = ozz->_animations.find(key);
    if (itr != ozz->_animations.end()) ozz->_animations.erase(itr);
    _animationFiles.erase(key); _animationDataList.erase(key);
}

std::vector<PlayerAnimation::ThisAndParent> PlayerAnimation::getSkeletonIndices(int from) const
{
    struct NameIterator
    {
        void operator()(int j, int p) { names.push_back(ThisAndParent(j, p)); }
        std::vector<ThisAndParent> names;
    } itr;

    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    itr = ozz::animation::IterateJointsDF(ozz->_skeleton, itr, from);
    return itr.names;
}

std::string PlayerAnimation::getSkeletonJointName(int j) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    return (j < ozz->_skeleton.num_joints()) ? ozz->_skeleton.joint_names()[j] : "";
}

int PlayerAnimation::getSkeletonJointIndex(const std::string& joint) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    auto names = ozz->_skeleton.joint_names();
    for (size_t i = 0; i < ozz->_skeleton.num_joints(); ++i)
    { if (names[i] == joint) return i; } return -1;
}

void PlayerAnimation::setModelSpaceJointMatrix(int joint, const osg::Matrix& matrix)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (joint < 0 || joint >= (int)ozz->_models.size()) return;

    ozz::math::Float4x4 m;
    for (int i = 0; i < 4; ++i)
    {
        m.cols[i] = ozz::math::simd_float4::Load(
            matrix(i, 0), matrix(i, 1), matrix(i, 2), matrix(i, 3));
    }
    ozz->_models[joint] = m;
}

osg::Matrix PlayerAnimation::getModelSpaceJointMatrix(int joint) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (joint < 0 || joint >= (int)ozz->_models.size()) return osg::Matrix();
    const ozz::math::Float4x4& m = ozz->_models[joint];
    return osg::Matrix(
        ozz::math::GetX(m.cols[0]), ozz::math::GetY(m.cols[0]), ozz::math::GetZ(m.cols[0]), ozz::math::GetW(m.cols[0]),
        ozz::math::GetX(m.cols[1]), ozz::math::GetY(m.cols[1]), ozz::math::GetZ(m.cols[1]), ozz::math::GetW(m.cols[1]),
        ozz::math::GetX(m.cols[2]), ozz::math::GetY(m.cols[2]), ozz::math::GetZ(m.cols[2]), ozz::math::GetW(m.cols[2]),
        ozz::math::GetX(m.cols[3]), ozz::math::GetY(m.cols[3]), ozz::math::GetZ(m.cols[3]), ozz::math::GetW(m.cols[3]));
}

std::vector<osg::Matrix> PlayerAnimation::getSkeletonRestPoseMatrices() const
{
    std::vector<osg::Matrix> result;
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    const int numJoints = ozz->_skeleton.num_joints();
    if (numJoints <= 0) return result;

    // Convert the local space rest poses to model space, just like computeSkeletonBounds() does
    ozz::vector<ozz::math::Float4x4> models(numJoints);
    ozz::animation::LocalToModelJob job;
    job.input = ozz->_skeleton.joint_rest_poses();
    job.output = ozz::make_span(models);
    job.skeleton = &(ozz->_skeleton);
    if (!job.Run()) return result;

    result.reserve(numJoints);
    for (int i = 0; i < numJoints; ++i)
        result.push_back(OzzAnimation::convertMatrix(models[i]));
    return result;
}

osg::BoundingBox PlayerAnimation::computeSkeletonBounds() const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    const int numJoints = ozz->_skeleton.num_joints();
    if (numJoints <= 0) return osg::BoundingBox();

    osg::BoundingBoxf bound;
    ozz::vector<ozz::math::Float4x4> models(numJoints);

    // Compute model space bind pose.
    ozz::animation::LocalToModelJob job;
    job.input = ozz->_skeleton.joint_rest_poses();
    job.output = ozz::make_span(models);
    job.skeleton = &(ozz->_skeleton);
    if (job.Run())
    {
        ozz::span<const ozz::math::Float4x4> matrices = job.output;
        if (matrices.empty()) return bound;

        const ozz::math::Float4x4* current = matrices.begin();
        ozz::math::SimdFloat4 minV = current->cols[3];
        ozz::math::SimdFloat4 maxV = current->cols[3]; ++current;
        while (current < matrices.end())
        {
            minV = ozz::math::Min(minV, current->cols[3]);
            maxV = ozz::math::Max(maxV, current->cols[3]); ++current;
        }

        ozz::math::Store3PtrU(minV, bound._min.ptr());
        ozz::math::Store3PtrU(maxV, bound._max.ptr());
    }
    return bound;
}

std::vector<std::string> PlayerAnimation::getAnimationNames() const
{
    std::vector<std::string> names;
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    std::map<std::string, OzzAnimation::AnimationSampler>::iterator itr;
    for (itr = ozz->_animations.begin(); itr != ozz->_animations.end(); ++itr) names.push_back(itr->first);
    return names;
}

float PlayerAnimation::getAnimationStartTime(const std::string& key)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for getAnimationStartTime(): "
                 << key << std::endl; return 0.0f;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    return sampler.startTime;
}

float PlayerAnimation::getAnimationWeight(const std::string& key) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for getAnimationWeight(): "
                 << key << std::endl; return 0.0f;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    return sampler.weight;
}

float PlayerAnimation::getTimeRatio(const std::string& key) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for getTimeRatio(): "
                 << key << std::endl; return 0.0f;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    return osg::clampBetween(sampler.timeRatio, 0.0f, 1.0f);
}

float PlayerAnimation::getDuration(const std::string& key) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for getDuration(): "
                 << key << std::endl; return 0.0f;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    return sampler.animation.duration();
}

float PlayerAnimation::getPlaybackSpeed(const std::string& key) const
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for getPlaybackSpeed(): "
                 << key << std::endl; return 0.0f;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    return sampler.playbackSpeed;
}

void PlayerAnimation::setPlaybackSpeed(const std::string& key, float s)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for setPlaybackSpeed(): "
                 << key << std::endl; return;
    }
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    sampler.playbackSpeed = s;
}

void PlayerAnimation::select(const std::string& key, float weight, bool looping, bool clearOthers)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for select(): "
                 << key << std::endl; return;
    }
    else if (clearOthers)
    {
        std::map<std::string, OzzAnimation::AnimationSampler>::iterator itr;
        for (itr = ozz->_animations.begin(); itr != ozz->_animations.end(); ++itr)
            itr->second.weight = 0.0f;
    }

    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    sampler.weight = weight; sampler.looping = looping;
    sampler.jointWeights.clear();  // clear specific joint weights
}

void PlayerAnimation::selectPartial(const std::string& key, float weight, bool looping,
                                    PlayerAnimation::SetJointWeightFunc func, void* userData)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for selectPartial(): "
                 << key << std::endl; return;
    }

    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    sampler.weight = weight; sampler.looping = looping;
    sampler.jointWeights.resize(sampler.locals.size());

    struct WeightSetIterator
    {
        typedef ozz::vector<ozz::math::SimdFloat4>* JointWeightPtr;
        JointWeightPtr weights; SetJointWeightFunc traveller; void* userData;
        WeightSetIterator(JointWeightPtr w, SetJointWeightFunc f, void* p)
            : weights(w), traveller(f), userData(p) {}

        void operator()(int j, int p)
        {
            float v = traveller(j, p, userData);
            ozz::math::SimdFloat4& soa = weights->at(j / 4);
            soa = ozz::math::SetI(soa, ozz::math::simd_float4::Load1(v), j % 4);
        }
    } itr(&(sampler.jointWeights), func, userData);
    ozz::animation::IterateJointsDF(ozz->_skeleton, itr, -1);
}

void PlayerAnimation::seek(const std::string& key, float timeRatio)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    if (ozz->_animations.find(key) == ozz->_animations.end())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation name for seek(): "
                 << key << std::endl; return;
    }

    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    sampler.timeRatio = osg::clampBetween(timeRatio, 0.0f, 1.0f);
    sampler.resetTimeRatio = true;
}

std::vector<std::string> PlayerAnimation::getBlendshapeNames() const
{
    std::vector<std::string> names;
    for (size_t i = 0; i < _blendshapes.size(); ++i)
    {
        osgVerse::BlendShapeAnimation* bs = _blendshapes[i].get();
        if (!bs) continue;  // It is common to have an empty BS callback...
        for (size_t j = 0; j < bs->getNumBlendShapes(); ++j)
            names.push_back(bs->getBlendShapeData(j)->name);
    }
    return names;
}

void PlayerAnimation::setBlendShape(const std::string& key, float weight)
{
    for (size_t i = 0; i < _blendshapes.size(); ++i)
    {
        BlendShapeAnimation::BlendShapeData* bsd = (!_blendshapes[i]) ? NULL
                                                 : _blendshapes[i]->getBlendShapeData(key);
        if (bsd != NULL) bsd->weight = weight;
    }
}

void PlayerAnimation::clearAllBlendShapes()
{
    for (size_t i = 0; i < _blendshapes.size(); ++i)
    {
        if (!_blendshapes[i]) continue;
        std::vector<osg::ref_ptr<BlendShapeAnimation::BlendShapeData>>& bsList =
            _blendshapes[i]->getAllBlendShapes();
        for (size_t j = 0; j < bsList.size(); ++j) bsList[j]->weight = 0.0f;
    }
}

bool PlayerAnimation::initializeInternal()
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    ozz->_models.resize(ozz->_skeleton.num_joints());
    ozz->_blended_locals.resize(ozz->_skeleton.num_soa_joints());

    size_t num_joints = ozz->_skeleton.num_joints(); size_t num_skinning_matrices = num_joints;
    for (const OzzMesh& mesh : ozz->_meshes)
        num_skinning_matrices = ozz::math::Max(num_skinning_matrices, mesh.joint_remaps.size());

    ozz->_skinning_matrices.resize(num_skinning_matrices, ozz::math::Float4x4::identity());
    for (const OzzMesh& mesh : ozz->_meshes)
    {
        if (num_joints < mesh.highest_joint_index())
        {
            ozz::log::Err() << "The provided mesh doesn't match skeleton "
                            << "(joint count mismatch)" << std::endl; return false;
        }
    }

    // Validate that the bind pose is self-consistent: at rest, every skinning matrix
    // (model matrix * inverse bind pose) must be the identity, otherwise the importer mixed
    // different coordinate spaces and the mesh would be deformed even without any animation
    ozz::vector<ozz::math::Float4x4> restModels(num_joints);
    ozz::animation::LocalToModelJob ltmJob;
    ltmJob.skeleton = &(ozz->_skeleton);
    ltmJob.input = ozz->_skeleton.joint_rest_poses();
    ltmJob.output = ozz::make_span(restModels);
    if (ltmJob.Run())
    {
        for (size_t i = 0; i < ozz->_meshes.size(); ++i)
        {
            const OzzMesh& mesh = ozz->_meshes[i];
            for (size_t j = 0; j < mesh.joint_remaps.size() && j < mesh.inverse_bind_poses.size(); ++j)
            {
                int joint = mesh.joint_remaps[j];
                ozz::math::Float4x4 m = restModels[joint] * mesh.inverse_bind_poses[j];
                float dev = 0.0f;
                for (int c = 0; c < 4; ++c)
                {
                    float e[4] = { (c == 0) ? 1.0f : 0.0f, (c == 1) ? 1.0f : 0.0f,
                                   (c == 2) ? 1.0f : 0.0f, (c == 3) ? 1.0f : 0.0f };
                    dev = osg::maximum(dev, std::abs(ozz::math::GetX(m.cols[c]) - e[0]));
                    dev = osg::maximum(dev, std::abs(ozz::math::GetY(m.cols[c]) - e[1]));
                    dev = osg::maximum(dev, std::abs(ozz::math::GetZ(m.cols[c]) - e[2]));
                    dev = osg::maximum(dev, std::abs(ozz::math::GetW(m.cols[c]) - e[3]));
                }
                if (dev > 0.01f)
                    OSG_WARN << "[PlayerAnimation] Mesh " << i << " joint " << joint << " ("
                             << ozz->_skeleton.joint_names()[joint] << ") has an inconsistent "
                             << "bind pose, deviation = " << dev << std::endl;
            }
        }
    }
    return true;
}

bool PlayerAnimation::loadAnimationInternal(const std::string& key)
{
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    OzzAnimation::AnimationSampler& sampler = ozz->_animations[key];
    if (sampler.animation.translations_values().empty() &&
        sampler.animation.rotations_values().empty() && sampler.animation.scales_values().empty())
    {
        OSG_WARN << "[PlayerAnimation] Invalid animation data: " << key << std::endl;
        ozz->_animations.erase(ozz->_animations.find(key)); return false;
    }

    const int num_joints = ozz->_skeleton.num_joints();
    if (num_joints != sampler.animation.num_tracks())
    {
        OSG_WARN << "[PlayerAnimation] The animation " << key << " failed to match skeleton. "
                 << "Joint count (" << num_joints << ") doesn't equal to animation tracks ("
                 << sampler.animation.num_tracks() << ")" << std::endl;
        ozz->_animations.erase(ozz->_animations.find(key)); return false;
    }

    sampler.locals.resize(ozz->_skeleton.num_soa_joints());
    ozz->_context.Resize(num_joints);
    if (ozz->_animations.size() > 1) sampler.weight = 0.0f;
    else sampler.weight = 1.0f;  // by default only the first animation is full weighted

#if false
    printAnimationData(ozz, key);
#endif
    return true;
}

PlayerAnimation::VertexWeights PlayerAnimation::getSkeletonVertexWeights() const
{
    VertexWeights weights;
    OzzAnimation* ozz = static_cast<OzzAnimation*>(_internal.get());
    for (size_t i = 0; i < ozz->_meshes.size(); ++i)
    {
        const OzzMesh& mesh = ozz->_meshes[i];
        for (size_t j = 0; j < mesh.parts.size(); ++j)
        {
            const OzzMesh::Part& part = mesh.parts[j];
            const ozz::vector<uint16_t>& jointIds = part.joint_indices;
            const ozz::vector<float>& jointWeights = part.joint_weights;
            for (size_t n = 0; n < part.vertex_count(); ++n)
            {
                size_t n0 = n * 4, n1 = n * 3;
                osg::Vec3 vec(part.positions[n1], part.positions[n1 + 1], part.positions[n1 + 2]);
                std::vector<JointAndWeight>& weightList = weights[vec];

                float w = jointWeights[n1] + jointWeights[n1 + 1] + jointWeights[n1 + 2];
                for (int k = 0; k < 4; ++k)
                {
                    // OZZ stores only 3 weights, the last one is implicitly 1 - sum
                    float weight = (k < 3) ? jointWeights[n1 + k] : (1.0f - w);
                    if (weight > 0.0f) weightList.push_back(
                        JointAndWeight(jointIds[n0 + k], weight));
                }
            }
        }
    }
    return weights;
}
