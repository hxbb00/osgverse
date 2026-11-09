#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <readerwriter/TileCallback.h>
using namespace osgVerse;

static bool checkLayerPaths(const TileCallback& cb)
{
    return !cb.getLayerPath(TileCallback::ELEVATION).empty()
        || !cb.getLayerPath(TileCallback::ORTHOPHOTO).empty()
        || !cb.getLayerPath(TileCallback::OCEAN_MASK).empty()
        || !cb.getLayerPath(TileCallback::USER).empty();
}

static bool readLayerPaths(osgDB::InputStream& is, TileCallback& cb)
{
    static const TileCallback::LayerType types[4] = {
        TileCallback::ELEVATION, TileCallback::ORTHOPHOTO, TileCallback::OCEAN_MASK, TileCallback::USER
    };
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size && i < 4; ++i)
    {
        std::string path; is.readWrappedString(path);
        cb.setLayerPath(types[i], path);
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeLayerPaths(osgDB::OutputStream& os, const TileCallback& cb)
{
    static const TileCallback::LayerType types[4] = {
        TileCallback::ELEVATION, TileCallback::ORTHOPHOTO, TileCallback::OCEAN_MASK, TileCallback::USER
    };
    os.writeSize(4); os << os.BEGIN_BRACKET << std::endl;
    for (int i = 0; i < 4; ++i)
    {
        os.writeWrappedString(cb.getLayerPath(types[i]));
        os << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(TileCallback,
                        new osgVerse::TileCallback,
                        osgVerse::TileCallback,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::TileCallback")
{
    ADD_USER_SERIALIZER(LayerPaths);
    ADD_VEC3D_SERIALIZER(MinExtent, osg::Vec3d(-180.0, -90.0, 0.0));
    ADD_VEC3D_SERIALIZER(MaxExtent, osg::Vec3d(180.0, 90.0, 0.0));
    ADD_INT_SERIALIZER(TileX, -1);
    ADD_INT_SERIALIZER(TileY, -1);
    ADD_INT_SERIALIZER(TileZ, -1);
    ADD_FLOAT_SERIALIZER(SkirtRatio, 0.02f);
    ADD_FLOAT_SERIALIZER(ElevationScale, 1.0f);
    ADD_BOOL_SERIALIZER(Flatten, true);
    ADD_BOOL_SERIALIZER(BottomLeft, false);
    ADD_BOOL_SERIALIZER(UseWebMercator, false);
    ADD_INT_SERIALIZER(PolarStereo, 0);
    ADD_BOOL_SERIALIZER(PolarBoundary, false);
    ADD_BOOL_SERIALIZER(LayersDone, false);
}
