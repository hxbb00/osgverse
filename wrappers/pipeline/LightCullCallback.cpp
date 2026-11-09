#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <pipeline/LightDrawable.h>
using namespace osgVerse;

/** The cull callback of LightDrawable is stateless (it registers the drawable into the
    global light manager), so it only needs to be re-created when loading a scene. */
REGISTER_OBJECT_WRAPPER(LightCullCallback,
                        new osgVerse::LightCullCallback,
                        osgVerse::LightCullCallback,
                        "osg::Object osg::Callback osg::NodeCallback osgVerse::LightCullCallback")
{
}
