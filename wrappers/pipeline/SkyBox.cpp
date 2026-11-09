#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/SkyBox.h>
using namespace osgVerse;

REGISTER_OBJECT_WRAPPER(SkyBox,
                        new osgVerse::SkyBox,
                        osgVerse::SkyBox,
                        "osg::Object osg::Node osg::Group osg::Transform osgVerse::SkyBox")
{
    ADD_OBJECT_SERIALIZER(VertexShader, osg::Shader, NULL);
    ADD_OBJECT_SERIALIZER(FragmentShader, osg::Shader, NULL);
}
