#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/PlayerAnimation.h>
#include <animation/BlendShapeAnimation.h>
using namespace osgVerse;

static void writeArray(osgDB::OutputStream& os, const osg::Array* array)
{
    os << (array != NULL);
    if (array != NULL) os << array;
    os << std::endl;
}

static osg::ref_ptr<osg::Array> readArray(osgDB::InputStream& is)
{
    bool hasArray = false; is >> hasArray;
    osg::ref_ptr<osg::Array> array;
    if (hasArray) is >> array;
    return array;
}

static void writePrimitiveSet(osgDB::OutputStream& os, const osg::PrimitiveSet* pset)
{
    os << (pset != NULL);
    if (pset != NULL) os << pset;
    os << std::endl;
}

static osg::ref_ptr<osg::PrimitiveSet> readPrimitiveSet(osgDB::InputStream& is)
{
    bool hasPset = false; is >> hasPset;
    osg::ref_ptr<osg::PrimitiveSet> pset;
    if (hasPset) is >> pset;
    return pset;
}

static void writeObject(osgDB::OutputStream& os, const osg::Object* obj)
{
    os << (obj != NULL);
    if (obj != NULL) os << obj;
    os << std::endl;
}

static osg::ref_ptr<osg::Object> readObject(osgDB::InputStream& is)
{
    bool hasObject = false; is >> hasObject;
    osg::ref_ptr<osg::Object> obj;
    if (hasObject) is >> obj;
    return obj;
}

static void writeAnimationData(osgDB::OutputStream& os, const PlayerAnimation::AnimationData& data)
{
    os.writeSize(data._interpolations.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<std::string, std::string>::const_iterator itr = data._interpolations.begin();
         itr != data._interpolations.end(); ++itr)
    { os.writeWrappedString(itr->first); os.writeWrappedString(itr->second); os << std::endl; }
    os << os.END_BRACKET << std::endl;

    os.writeSize(data._morphFrames.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < data._morphFrames.size(); ++i)
        os << data._morphFrames[i].first << data._morphFrames[i].second << std::endl;
    os << os.END_BRACKET << std::endl;

    os.writeSize(data._positionFrames.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < data._positionFrames.size(); ++i)
        os << data._positionFrames[i].first << data._positionFrames[i].second << std::endl;
    os << os.END_BRACKET << std::endl;

    os.writeSize(data._scaleFrames.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < data._scaleFrames.size(); ++i)
        os << data._scaleFrames[i].first << data._scaleFrames[i].second << std::endl;
    os << os.END_BRACKET << std::endl;

    os.writeSize(data._rotationFrames.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < data._rotationFrames.size(); ++i)
        os << data._rotationFrames[i].first << data._rotationFrames[i].second << std::endl;
    os << os.END_BRACKET << std::endl;
}

static bool readAnimationData(osgDB::InputStream& is, PlayerAnimation::AnimationData& data)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        std::string key, value; is.readWrappedString(key); is.readWrappedString(value);
        data._interpolations[key] = value;
    }
    is >> is.END_BRACKET;

    size = is.readSize(); is >> is.BEGIN_BRACKET;
    data._morphFrames.resize(size);
    for (unsigned int i = 0; i < size; ++i)
        is >> data._morphFrames[i].first >> data._morphFrames[i].second;
    is >> is.END_BRACKET;

    size = is.readSize(); is >> is.BEGIN_BRACKET;
    data._positionFrames.resize(size);
    for (unsigned int i = 0; i < size; ++i)
        is >> data._positionFrames[i].first >> data._positionFrames[i].second;
    is >> is.END_BRACKET;

    size = is.readSize(); is >> is.BEGIN_BRACKET;
    data._scaleFrames.resize(size);
    for (unsigned int i = 0; i < size; ++i)
        is >> data._scaleFrames[i].first >> data._scaleFrames[i].second;
    is >> is.END_BRACKET;

    size = is.readSize(); is >> is.BEGIN_BRACKET;
    data._rotationFrames.resize(size);
    for (unsigned int i = 0; i < size; ++i)
        is >> data._rotationFrames[i].first >> data._rotationFrames[i].second;
    is >> is.END_BRACKET;
    return true;
}

static bool checkPlayerData(const PlayerAnimation& anim) { return true; }

static bool readPlayerData(osgDB::InputStream& is, PlayerAnimation& anim)
{
    bool animated = true, drawSkeleton = true, drawSkinning = true;
    bool restPose = false, external = false; float threshold = 0.0f;
    is >> is.PROPERTY("Flags") >> animated >> drawSkeleton >> drawSkinning >> restPose >> external;
    is >> is.PROPERTY("BlendingThreshold") >> threshold;

    std::string skeletonFile, meshFile;
    is >> is.PROPERTY("SkeletonFile"); is.readWrappedString(skeletonFile);
    is >> is.PROPERTY("MeshFile"); is.readWrappedString(meshFile);

    unsigned int numJoints = is.readSize(); is >> is.BEGIN_BRACKET;
    std::vector<PlayerAnimation::JointBinding> joints(numJoints);
    for (unsigned int i = 0; i < numJoints; ++i)
    {
        PlayerAnimation::JointBinding& j = joints[i];
        is.readWrappedString(j.name);
        is >> j.parent >> j.translation >> j.scale >> j.rotation;
    }
    is >> is.END_BRACKET;

    unsigned int numMeshes = is.readSize(); is >> is.BEGIN_BRACKET;
    std::vector<PlayerAnimation::MeshBinding> meshes(numMeshes);
    for (unsigned int i = 0; i < numMeshes; ++i)
    {
        PlayerAnimation::MeshBinding& m = meshes[i];
        is.readWrappedString(m.name);
        m.positions = dynamic_cast<osg::Vec3Array*>(readArray(is).get());
        m.normals = dynamic_cast<osg::Vec3Array*>(readArray(is).get());
        m.tangents = dynamic_cast<osg::Vec4Array*>(readArray(is).get());
        m.uvs = dynamic_cast<osg::Vec2Array*>(readArray(is).get());
        m.colors = dynamic_cast<osg::Vec4ubArray*>(readArray(is).get());
        m.indices = dynamic_cast<osg::DrawElementsUInt*>(readPrimitiveSet(is).get());

        unsigned int numIndices = is.readSize(); is >> is.BEGIN_BRACKET;
        m.jointIndices.resize(numIndices);
        for (unsigned int k = 0; k < numIndices; ++k) is >> m.jointIndices[k];
        is >> is.END_BRACKET;

        unsigned int numWeights = is.readSize(); is >> is.BEGIN_BRACKET;
        m.jointWeights.resize(numWeights);
        for (unsigned int k = 0; k < numWeights; ++k) is >> m.jointWeights[k];
        is >> is.END_BRACKET;

        unsigned int numPoses = is.readSize(); is >> is.BEGIN_BRACKET;
        m.inverseBindPoses.resize(numPoses);
        for (unsigned int k = 0; k < numPoses; ++k) is >> m.inverseBindPoses[k];
        is >> is.END_BRACKET;

        m.stateSet = dynamic_cast<osg::StateSet*>(readObject(is).get());
        m.blendshape = dynamic_cast<BlendShapeAnimation*>(readObject(is).get());
    }
    is >> is.END_BRACKET;

    unsigned int numAnimations = is.readSize(); is >> is.BEGIN_BRACKET;
    std::map<std::string, std::string> animationFiles;
    for (unsigned int i = 0; i < numAnimations; ++i)
    {
        std::string key, file; is.readWrappedString(key); is.readWrappedString(file);
        animationFiles[key] = file;
    }
    is >> is.END_BRACKET;

    unsigned int numAnimationData = is.readSize(); is >> is.BEGIN_BRACKET;
    std::map<std::string, std::vector<PlayerAnimation::AnimationData>> animationData;
    for (unsigned int i = 0; i < numAnimationData; ++i)
    {
        std::string key; is.readWrappedString(key);
        unsigned int numTracks = is.readSize(); is >> is.BEGIN_BRACKET;
        std::vector<PlayerAnimation::AnimationData>& tracks = animationData[key];
        tracks.resize(numTracks);
        for (unsigned int j = 0; j < numTracks; ++j) readAnimationData(is, tracks[j]);
        is >> is.END_BRACKET;
    }
    is >> is.END_BRACKET;

    // Rebuild the player from the stored bindings
    bool ok = false;
    if (!skeletonFile.empty() && !meshFile.empty())
        ok = anim.initialize(skeletonFile, meshFile);
    else if (!joints.empty() && !meshes.empty())
        ok = anim.initialize(joints, meshes);
    if (!ok)
        OSG_WARN << "[PlayerAnimation] Failed to rebuild skeleton/mesh from serialized data" << std::endl;

    // Restore embedded animation tracks first (no original files needed), then file-based ones
    std::map<std::string, std::string>& animFiles = anim.getAnimationFiles();
    animFiles.clear();
    for (std::map<std::string, std::vector<PlayerAnimation::AnimationData>>::iterator
         itr = animationData.begin(); itr != animationData.end(); ++itr)
        anim.loadAnimation(itr->first, itr->second);
    for (std::map<std::string, std::string>::iterator itr = animationFiles.begin();
         itr != animationFiles.end(); ++itr)
    {
        if (animationData.find(itr->first) != animationData.end()) continue;
        if (anim.loadAnimation(itr->first, itr->second))
            animFiles[itr->first] = itr->second;
    }

    anim.setPlaying(animated, restPose);
    anim.setDrawingSkeleton(drawSkeleton); anim.setDrawingSkinning(drawSkinning);
    anim.setSkeletonDrivenExternally(external);
    anim.setBlendingThreshold(threshold);
    return true;
}

static bool writePlayerData(osgDB::OutputStream& os, const PlayerAnimation& anim)
{
    bool restPose = false, animated = anim.getPlaying(&restPose);
    os << os.PROPERTY("Flags") << animated << anim.getDrawingSkeleton()
                                  << anim.getDrawingSkinning() << restPose
                                  << anim.getSkeletonDrivenExternally() << std::endl;
    os << os.PROPERTY("BlendingThreshold") << anim.getBlendingThreshold() << std::endl;

    os << os.PROPERTY("SkeletonFile"); os.writeWrappedString(anim.getSkeletonFile()); os << std::endl;
    os << os.PROPERTY("MeshFile"); os.writeWrappedString(anim.getMeshFile()); os << std::endl;

    const std::vector<PlayerAnimation::JointBinding>& joints = anim.getJointBindings();
    os.writeSize(joints.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const PlayerAnimation::JointBinding& j = joints[i];
        os.writeWrappedString(j.name);
        os << j.parent << j.translation << j.scale << j.rotation << std::endl;
    }
    os << os.END_BRACKET << std::endl;

    const std::vector<PlayerAnimation::MeshBinding>& meshes = anim.getMeshBindings();
    os.writeSize(meshes.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        const PlayerAnimation::MeshBinding& m = meshes[i];
        os.writeWrappedString(m.name); os << std::endl;
        writeArray(os, m.positions.get());
        writeArray(os, m.normals.get());
        writeArray(os, m.tangents.get());
        writeArray(os, m.uvs.get());
        writeArray(os, m.colors.get());
        writePrimitiveSet(os, m.indices.get());

        os.writeSize(m.jointIndices.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t k = 0; k < m.jointIndices.size(); ++k) os << m.jointIndices[k] << std::endl;
        os << os.END_BRACKET << std::endl;

        os.writeSize(m.jointWeights.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t k = 0; k < m.jointWeights.size(); ++k) os << m.jointWeights[k] << std::endl;
        os << os.END_BRACKET << std::endl;

        os.writeSize(m.inverseBindPoses.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t k = 0; k < m.inverseBindPoses.size(); ++k) os << m.inverseBindPoses[k] << std::endl;
        os << os.END_BRACKET << std::endl;

        writeObject(os, m.stateSet.get());
        writeObject(os, m.blendshape.get());
    }
    os << os.END_BRACKET << std::endl;

    const std::map<std::string, std::string>& animationFiles = anim.getAnimationFiles();
    os.writeSize(animationFiles.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<std::string, std::string>::const_iterator itr = animationFiles.begin();
         itr != animationFiles.end(); ++itr)
    {
        os.writeWrappedString(itr->first);
        os.writeWrappedString(itr->second);
        os << std::endl;
    }
    os << os.END_BRACKET << std::endl;

    const std::map<std::string, std::vector<PlayerAnimation::AnimationData>>& dataList =
        anim.getAnimationDataList();
    os.writeSize(dataList.size()); os << os.BEGIN_BRACKET << std::endl;
    for (std::map<std::string, std::vector<PlayerAnimation::AnimationData>>::const_iterator
         itr = dataList.begin(); itr != dataList.end(); ++itr)
    {
        os.writeWrappedString(itr->first); os << std::endl;
        os.writeSize(itr->second.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t i = 0; i < itr->second.size(); ++i) writeAnimationData(os, itr->second[i]);
        os << os.END_BRACKET << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(PlayerAnimation,
                        new osgVerse::PlayerAnimation,
                        osgVerse::PlayerAnimation,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::PlayerAnimation")
{
    ADD_USER_SERIALIZER(PlayerData);
}
