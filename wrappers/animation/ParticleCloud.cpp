#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/ParticleEngine.h>
using namespace osgVerse;

REGISTER_OBJECT_WRAPPER(ParticleCloud,
                        new osgVerse::ParticleCloud,
                        osgVerse::ParticleCloud,
                        "osg::Object osgVerse::ParticleCloud")
{
    ADD_OBJECT_SERIALIZER(Positions, osg::Vec4Array, NULL);
    ADD_OBJECT_SERIALIZER(Velocities, osg::Vec4Array, NULL);
    ADD_OBJECT_SERIALIZER(Colors, osg::Vec4Array, NULL);
    ADD_OBJECT_SERIALIZER(Attributes, osg::Vec4Array, NULL);
}
