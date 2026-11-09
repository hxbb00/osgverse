#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/LightDrawable.h>
using namespace osgVerse;

REGISTER_OBJECT_WRAPPER(LightDrawable,
                        new osgVerse::LightDrawable,
                        osgVerse::LightDrawable,
                        "osg::Object osg::Node osg::Drawable osg::ShapeDrawable osgVerse::LightDrawable")
{
    ADD_VEC3_SERIALIZER(Color, osg::Vec3(1.0f, 1.0f, 1.0f));
    ADD_VEC3_SERIALIZER(Position, osg::Vec3(0.0f, 0.0f, 1.0f));
    ADD_VEC3_SERIALIZER(Direction, osg::Vec3(1.0f, 0.0f, 0.0f));
    ADD_BOOL_SERIALIZER(Directional, false);
    ADD_BOOL_SERIALIZER(EyeSpace, false);
    ADD_BOOL_SERIALIZER(DebugShow, false);
    ADD_FLOAT_SERIALIZER(Range, 0.0f);
    ADD_FLOAT_SERIALIZER(SpotCutoff, 0.0f);
    ADD_FLOAT_SERIALIZER(DebugSizeScale, 0.1f);
}
