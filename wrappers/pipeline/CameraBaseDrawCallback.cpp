#include <osg/Camera>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>

namespace wrap_osgCameraDrawCallback
{
    // OSG does not ship a wrapper for osg::Camera::DrawCallback, which makes every
    // osgVerse callback deriving from it report "Unsupported associated class"
    // when saving scenes. Registering an empty base wrapper fixes that
    REGISTER_OBJECT_WRAPPER(CameraBaseDrawCallback,
                            new osg::Camera::DrawCallback,
                            osg::Camera::DrawCallback,
                            "osg::Object osg::Callback osg::Camera::DrawCallback")
    {
    }
}
