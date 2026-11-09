#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/ParticleEngine.h>
using namespace osgVerse;

template<typename T>
static void writeMap(osgDB::OutputStream& os, const char* name, const std::map<float, T>& m)
{
    os << os.PROPERTY(name) << (unsigned int)m.size() << std::endl;
    os << os.BEGIN_BRACKET << std::endl;
    for (typename std::map<float, T>::const_iterator itr = m.begin(); itr != m.end(); ++itr)
        os << itr->first << itr->second << std::endl;
    os << os.END_BRACKET << std::endl;
}

template<typename T>
static void readMap(osgDB::InputStream& is, const char* name, std::map<float, T>& m)
{
    unsigned int size = 0; is >> is.PROPERTY(name) >> size; is >> is.BEGIN_BRACKET;
    m.clear();
    for (unsigned int i = 0; i < size; ++i)
    {
        float t = 0.0f; T value; is >> t >> value; m[t] = value;
    }
    is >> is.END_BRACKET;
}

static bool checkConfiguration(const ParticleSystemU3D& ps) { return true; }

static bool readConfiguration(osgDB::InputStream& is, ParticleSystemU3D& ps)
{
    int method = 0, particleType = 0, blending = 0, emissionShape = 0, emissionSurface = 0;
    is >> is.PROPERTY("Method") >> method;
    is >> is.PROPERTY("Types") >> particleType >> blending >> emissionShape >> emissionSurface;
    ps.setUpdateMethod((ParticleSystemU3D::UpdateMethod)method);
    ps.setParticleType((ParticleSystemU3D::ParticleType)particleType);
    ps.setBlendingType((ParticleSystemU3D::BlendingType)blending);
    ps.setEmissionShape((ParticleSystemU3D::EmissionShape)emissionShape);
    ps.setEmissionSurface((ParticleSystemU3D::EmissionSurface)emissionSurface);

    osg::Vec3 center, eulers; osg::Vec4 values, collisionValues, sheetRange, sheetValues;
    osg::Vec2 count, sheetTiles;
    is >> is.PROPERTY("EmissionShapeData") >> center >> eulers >> values >> count;
    is >> is.PROPERTY("Collision") >> collisionValues;
    is >> is.PROPERTY("TextureSheet") >> sheetRange >> sheetValues >> sheetTiles;
    ps.setEmissionShapeCenter(center); ps.setEmissionShapeEulers(eulers);
    ps.setEmissionShapeValues(values); ps.setEmissionCount(count);
    ps.setCollisionValues(collisionValues);
    ps.setTextureSheetRange(sheetRange); ps.setTextureSheetValues(sheetValues);
    ps.setTextureSheetTiles(sheetTiles);

    unsigned int planeCount = 0; is >> is.PROPERTY("CollisionPlanes") >> planeCount;
    is >> is.BEGIN_BRACKET;
    std::vector<osg::Plane> planes(planeCount);
    for (unsigned int i = 0; i < planeCount; ++i)
    {
        osg::Vec4 v; is >> v; planes[i] = osg::Plane(v[0], v[1], v[2], v[3]);
    }
    is >> is.END_BRACKET; ps.setCollisionPlanes(planes);

    readMap(is, "EmissionBursts", ps.getEmissionBursts());
    readMap(is, "ColorPerTime", ps.getColorPerTime());
    readMap(is, "ColorPerSpeed", ps.getColorPerSpeed());
    readMap(is, "EulersPerTime", ps.getEulersPerTime());
    readMap(is, "EulersPerSpeed", ps.getEulersPerSpeed());
    readMap(is, "VelocityPerTime", ps.getVelocityPerTime());
    readMap(is, "ForcePerTime", ps.getForcePerTime());
    readMap(is, "ScalePerTime", ps.getScalePerTime());
    readMap(is, "ScalePerSpeed", ps.getScalePerSpeed());

    osg::ref_ptr<ParticleCloud> cloud; bool immutable = false;
    is >> is.PROPERTY("PointCloud") >> cloud;
    is >> is.PROPERTY("Immutable") >> immutable;
    if (cloud.valid()) ps.setPointCloud(cloud.get(), immutable);
    return true;
}

static bool writeConfiguration(osgDB::OutputStream& os, const ParticleSystemU3D& ps)
{
    os << os.PROPERTY("Method") << (int)ps.getUpdateMethod() << std::endl;
    os << os.PROPERTY("Types") << (int)ps.getParticleType() << (int)ps.getBlendingType()
                               << (int)ps.getEmissionShape() << (int)ps.getEmissionSurface() << std::endl;
    os << os.PROPERTY("EmissionShapeData") << ps.getEmissionShapeCenter() << ps.getEmissionShapeEulers()
                                          << ps.getEmissionShapeValues() << ps.getEmissionCount() << std::endl;
    os << os.PROPERTY("Collision") << ps.getCollisionValues() << std::endl;
    os << os.PROPERTY("TextureSheet") << ps.getTextureSheetRange() << ps.getTextureSheetValues()
                                      << ps.getTextureSheetTiles() << std::endl;

    const std::vector<osg::Plane>& planes = ps.getCollisionPlanes();
    os << os.PROPERTY("CollisionPlanes") << (unsigned int)planes.size() << std::endl;
    os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < planes.size(); ++i) os << planes[i].asVec4() << std::endl;
    os << os.END_BRACKET << std::endl;

    writeMap(os, "EmissionBursts", ps.getEmissionBursts());
    writeMap(os, "ColorPerTime", ps.getColorPerTime());
    writeMap(os, "ColorPerSpeed", ps.getColorPerSpeed());
    writeMap(os, "EulersPerTime", ps.getEulersPerTime());
    writeMap(os, "EulersPerSpeed", ps.getEulersPerSpeed());
    writeMap(os, "VelocityPerTime", ps.getVelocityPerTime());
    writeMap(os, "ForcePerTime", ps.getForcePerTime());
    writeMap(os, "ScalePerTime", ps.getScalePerTime());
    writeMap(os, "ScalePerSpeed", ps.getScalePerSpeed());

    os << os.PROPERTY("PointCloud") << ps.getPointCloud() << std::endl;
    os << os.PROPERTY("Immutable") << ps.isImmutable() << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(ParticleSystemU3D,
                        new osgVerse::ParticleSystemU3D,
                        osgVerse::ParticleSystemU3D,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::ParticleSystemU3D")
{
    ADD_VEC3_SERIALIZER(StartDirection, osg::Vec3());
    ADD_VEC2_SERIALIZER(StartLifeRange, osg::Vec2());
    ADD_VEC2_SERIALIZER(StartSizeRange, osg::Vec2());
    ADD_VEC2_SERIALIZER(StartSpeedRange, osg::Vec2());
    ADD_QUAT_SERIALIZER(StartAttitude0, osg::Quat());
    ADD_QUAT_SERIALIZER(StartAttitude1, osg::Quat());
    ADD_DOUBLE_SERIALIZER(MaxParticles, 0.0);
    ADD_DOUBLE_SERIALIZER(StartDelay, 0.0);
    ADD_DOUBLE_SERIALIZER(GravityScale, 1.0);
    ADD_DOUBLE_SERIALIZER(Duration, 0.0);
    ADD_DOUBLE_SERIALIZER(AspectRatio, 1.0);
    ADD_OBJECT_SERIALIZER(Texture, osg::Texture2D, NULL);
    ADD_OBJECT_SERIALIZER(Geometry, osg::Geometry, NULL);
    ADD_USER_SERIALIZER(Configuration);
}
