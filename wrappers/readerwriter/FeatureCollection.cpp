#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <readerwriter/FeatureDefinition.h>
using namespace osgVerse;

static bool checkFeatureCollection(const FeatureCollection& collection)
{ return !collection.features.empty(); }

static bool readFeatureCollection(osgDB::InputStream& is, FeatureCollection& collection)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        osg::ref_ptr<Feature> feature = is.readObjectOfType<Feature>();
        if (feature.valid()) collection.push_back(feature.get());
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeFeatureCollection(osgDB::OutputStream& os, const FeatureCollection& collection)
{
    os.writeSize(collection.features.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < collection.features.size(); ++i)
        os << collection.features[i].get() << std::endl;
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(FeatureCollection,
                        new osgVerse::FeatureCollection,
                        osgVerse::FeatureCollection,
                        "osg::Object osgVerse::FeatureCollection")
{
    ADD_USER_SERIALIZER(FeatureCollection);
}
