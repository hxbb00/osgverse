#include <osgDB/ObjectWrapper>
#include <readerwriter/TileCallback.h>
using namespace osgVerse;

REGISTER_OBJECT_WRAPPER(TileGeometryHandler,
                        new osgVerse::TileGeometryHandler,
                        osgVerse::TileGeometryHandler,
                        "osg::Object osgVerse::TileGeometryHandler")
{
}
