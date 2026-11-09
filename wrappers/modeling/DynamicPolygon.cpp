#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <modeling/DynamicGeometry.h>
using namespace osgVerse;

static bool checkState(const DynamicPolygon& geom) { return true; }

static bool readState(osgDB::InputStream& is, DynamicPolygon& geom)
{
    int mode = 0; bool editing = false;
    is >> is.PROPERTY("Mode") >> mode;
    is >> is.PROPERTY("Editing") >> editing;
    geom.setMode((GLenum)mode); geom.setEditing(editing); return true;
}

static bool writeState(osgDB::OutputStream& os, const DynamicPolygon& geom)
{
    os << os.PROPERTY("Mode") << (int)geom.getMode() << std::endl;
    os << os.PROPERTY("Editing") << geom.isEditing() << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(DynamicPolygon,
                        new osgVerse::DynamicPolygon,
                        osgVerse::DynamicPolygon,
                        "osg::Object osg::Node osg::Drawable osg::Geometry osgVerse::DynamicPolygon")
{
    ADD_USER_SERIALIZER(State);
}
