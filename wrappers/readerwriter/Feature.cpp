#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <readerwriter/FeatureDefinition.h>
using namespace osgVerse;

static bool checkFeature(const Feature& feature)
{ return !feature.getPointList().empty() || feature.getType() != GL_NONE; }

static bool readFeature(osgDB::InputStream& is, Feature& feature)
{
    int type = (int)GL_NONE;
    is >> is.PROPERTY("Type") >> type;
    feature.setType((GLenum)type);

    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        osg::ref_ptr<osg::Vec3Array> points = is.readObjectOfType<osg::Vec3Array>();
        if (points.valid()) feature.addPoints(points.get());
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeFeature(osgDB::OutputStream& os, const Feature& feature)
{
    os << os.PROPERTY("Type") << (int)feature.getType() << std::endl;

    const std::vector<osg::ref_ptr<osg::Vec3Array>>& pointList = feature.getPointList();
    os.writeSize(pointList.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < pointList.size(); ++i) os << pointList[i].get() << std::endl;
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(Feature,
                        new osgVerse::Feature,
                        osgVerse::Feature,
                        "osg::Object osgVerse::Feature")
{
    ADD_USER_SERIALIZER(Feature);
}
