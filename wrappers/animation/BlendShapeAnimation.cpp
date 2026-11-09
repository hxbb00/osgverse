#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <animation/BlendShapeAnimation.h>
using namespace osgVerse;

static void writeArray(osgDB::OutputStream& os, const osg::Array* array)
{
    os << (array != NULL);
    if (array != NULL) os << array;
    os << std::endl;
}

static osg::ref_ptr<osg::Array> readArray(osgDB::InputStream& is)
{
    bool hasArray = false; is >> hasArray;
    osg::ref_ptr<osg::Array> array;
    if (hasArray) is >> array;
    return array;
}

static bool checkBlendShapes(const BlendShapeAnimation& anim)
{ return anim.getNumBlendShapes() > 0; }

static bool readBlendShapes(osgDB::InputStream& is, BlendShapeAnimation& anim)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    anim.clearBlendShapeData();
    for (unsigned int i = 0; i < size; ++i)
    {
        std::string name; is.readWrappedString(name);
        double weight = 0.0; is >> weight;

        BlendShapeAnimation::BlendShapeData* bsd = new BlendShapeAnimation::BlendShapeData(weight);
        osg::ref_ptr<osg::Array> verts = readArray(is);
        osg::ref_ptr<osg::Array> norms = readArray(is);
        osg::ref_ptr<osg::Array> tans = readArray(is);
        bsd->vertices = dynamic_cast<osg::Vec3Array*>(verts.get());
        bsd->normals = dynamic_cast<osg::Vec3Array*>(norms.get());
        bsd->tangents = dynamic_cast<osg::Vec4Array*>(tans.get());
        anim.addBlendShapeData(bsd);
        if (!name.empty()) anim.registerBlendShape(name, bsd);
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writeBlendShapes(osgDB::OutputStream& os, const BlendShapeAnimation& anim)
{
    os.writeSize(anim.getNumBlendShapes()); os << os.BEGIN_BRACKET << std::endl;
    const std::vector<osg::ref_ptr<BlendShapeAnimation::BlendShapeData>>& list = anim.getAllBlendShapes();
    for (unsigned int i = 0; i < anim.getNumBlendShapes(); ++i)
    {
        const BlendShapeAnimation::BlendShapeData* bsd = list[i].get();
        os.writeWrappedString(bsd->name); os << std::endl;
        os << bsd->weight << std::endl;
        writeArray(os, bsd->vertices.get());
        writeArray(os, bsd->normals.get());
        writeArray(os, bsd->tangents.get());
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(BlendShapeAnimation,
                        new osgVerse::BlendShapeAnimation,
                        osgVerse::BlendShapeAnimation,
                        "osg::Object osg::Callback osg::UpdateCallback osgVerse::BlendShapeAnimation")
{
    ADD_USER_SERIALIZER(BlendShapes);
}
