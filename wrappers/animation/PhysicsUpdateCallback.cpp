#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/Utilities.h>
using namespace osgVerse;

REGISTER_OBJECT_WRAPPER(PhysicsUpdateCallback,
                        new osgVerse::PhysicsUpdateCallback,
                        osgVerse::PhysicsUpdateCallback,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::PhysicsUpdateCallback")
{
    ADD_OBJECT_SERIALIZER(Engine, osgVerse::PhysicsEngine, NULL);
    ADD_STRING_SERIALIZER(BodyName, "");
}

#undef OBJECT_CAST
#define OBJECT_CAST static_cast
