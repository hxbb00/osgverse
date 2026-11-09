#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <modeling/DynamicGeometry.h>
using namespace osgVerse;

static bool checkState(const DynamicPolyline& geom) { return true; }

static bool readState(osgDB::InputStream& is, DynamicPolyline& geom)
{
    int mode = 0; bool editing = false;
    is >> is.PROPERTY("Mode") >> mode;
    is >> is.PROPERTY("Editing") >> editing;
    geom.setMode((GLenum)mode); geom.setEditing(editing); return true;
}

static bool writeState(osgDB::OutputStream& os, const DynamicPolyline& geom)
{
    os << os.PROPERTY("Mode") << (int)geom.getMode() << std::endl;
    os << os.PROPERTY("Editing") << geom.isEditing() << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(DynamicPolyline,
                        new osgVerse::DynamicPolyline,
                        osgVerse::DynamicPolyline,
                        "osg::Object osg::Node osg::Drawable osg::Geometry osgVerse::DynamicPolyline")
{
    ADD_USER_SERIALIZER(State);
}
