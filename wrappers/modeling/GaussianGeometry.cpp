#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <modeling/GaussianGeometry.h>
using namespace osgVerse;

static void writeArray(osgDB::OutputStream& os, const char* name, const osg::Array* array)
{
    os << os.PROPERTY(name) << (array != NULL);
    if (array != NULL) os << array; else os << std::endl;
}

static osg::ref_ptr<osg::Array> readArray(osgDB::InputStream& is, const char* name)
{
    bool hasArray = false; is >> is.PROPERTY(name) >> hasArray;
    osg::ref_ptr<osg::Array> array;
    if (hasArray) is >> array;
    return array;
}

static bool checkGaussianGeometry(const GaussianGeometry& geom)
{ return geom.getNumSplats() > 0; }

static bool readGaussianGeometry(osgDB::InputStream& is, GaussianGeometry& geom)
{
    int method = 0, degrees = 0, numSplats = 0;
    is >> is.PROPERTY("RenderMethod") >> method;
    is >> is.PROPERTY("ShDegrees") >> degrees;
    is >> is.PROPERTY("NumSplats") >> numSplats;
    geom.setRenderMethod((GaussianGeometry::RenderMethod)method);
    geom.setShDegrees(degrees); geom.setNumSplats(numSplats);

    // The geometry-shader mode keeps everything in vertex attributes, which are already
    // handled by the inherited osg::Geometry serializer
    if (method == GaussianGeometry::GEOMETRY_SHADER) return true;

    osg::ref_ptr<osg::Array> posArray = readArray(is, "Position");
    osg::ref_ptr<osg::Array> scaleArray = readArray(is, "Scale");
    osg::ref_ptr<osg::Array> rotArray = readArray(is, "Rotation");
    osg::ref_ptr<osg::Array> alphaArray = readArray(is, "Alpha");

    osg::Vec4Array* pos4 = dynamic_cast<osg::Vec4Array*>(posArray.get());
    osg::Vec3Array* scale = dynamic_cast<osg::Vec3Array*>(scaleArray.get());
    osg::Vec4Array* rotation = dynamic_cast<osg::Vec4Array*>(rotArray.get());
    osg::FloatArray* alpha = dynamic_cast<osg::FloatArray*>(alphaArray.get());
    if (pos4 != NULL)
    {
        osg::ref_ptr<osg::Vec3Array> pos3 = new osg::Vec3Array(pos4->size());
        for (size_t i = 0; i < pos4->size(); ++i)
            (*pos3)[i] = osg::Vec3((*pos4)[i][0], (*pos4)[i][1], (*pos4)[i][2]);
        geom.setPosition(pos3.get());
    }
    if (scale != NULL && rotation != NULL) geom.setScaleAndRotation(scale, rotation, alpha);

    for (int i = 0; i < 4; ++i)
    {
        std::string prefix = "ShRed" + std::to_string(i);
        osg::ref_ptr<osg::Array> red = readArray(is, prefix.c_str());
        prefix = "ShGreen" + std::to_string(i);
        osg::ref_ptr<osg::Array> green = readArray(is, prefix.c_str());
        prefix = "ShBlue" + std::to_string(i);
        osg::ref_ptr<osg::Array> blue = readArray(is, prefix.c_str());

        osg::Vec4Array* r = dynamic_cast<osg::Vec4Array*>(red.get());
        osg::Vec4Array* g = dynamic_cast<osg::Vec4Array*>(green.get());
        osg::Vec4Array* b = dynamic_cast<osg::Vec4Array*>(blue.get());
        if (r != NULL) geom.setShRed(i, r);
        if (g != NULL) geom.setShGreen(i, g);
        if (b != NULL) geom.setShBlue(i, b);
    }
    if (!geom.finalize())
        OSG_WARN << "[GaussianGeometry] Failed to finalize deserialized splats" << std::endl;
    return true;
}

static bool writeGaussianGeometry(osgDB::OutputStream& os, const GaussianGeometry& geom)
{
    int method = (int)geom.getRenderMethod();
    os << os.PROPERTY("RenderMethod") << method << std::endl;
    os << os.PROPERTY("ShDegrees") << geom.getShDegrees() << std::endl;
    os << os.PROPERTY("NumSplats") << geom.getNumSplats() << std::endl;
    if (method == GaussianGeometry::GEOMETRY_SHADER) return true;

    GaussianGeometry& g = const_cast<GaussianGeometry&>(geom);
    int num = geom.getNumSplats();

    // All arrays must stay alive until the whole function returns: osgDB::OutputStream
    // identifies arrays by their pointer, and a freed temporary may be reused for the
    // next getter result, making the stream reference the previous (wrong) array.
    std::vector<osg::ref_ptr<osg::Array> > arrays;

    osg::ref_ptr<osg::Vec4Array> pos = new osg::Vec4Array(num);
    osg::Vec4* posData = g.getPosition4();
    if (posData != NULL) memcpy(&(*pos)[0], posData, num * sizeof(osg::Vec4));
    arrays.push_back(pos.get());
    writeArray(os, "Position", pos.get());

    osg::ref_ptr<osg::Vec3Array> scale = g.getScale();
    osg::ref_ptr<osg::Vec4Array> rotation = g.getRotation();
    osg::ref_ptr<osg::FloatArray> alpha = g.getAlpha();
    if (rotation.valid())
    {   // getRotation() returns (w, x, y, z) to fit GaussForge, while setScaleAndRotation()
        // expects (x, y, z, w): reorder here so the serialized data stays symmetric
        for (size_t i = 0; i < rotation->size(); ++i)
        {
            const osg::Vec4& q = (*rotation)[i];
            (*rotation)[i] = osg::Vec4(q.y(), q.z(), q.w(), q.x());
        }
    }
    arrays.push_back(scale.get()); arrays.push_back(rotation.get()); arrays.push_back(alpha.get());
    writeArray(os, "Scale", scale.get());
    writeArray(os, "Rotation", rotation.get());
    writeArray(os, "Alpha", alpha.get());

    for (int i = 0; i < 4; ++i)
    {
        osg::ref_ptr<osg::Vec4Array> shRed = g.getShRed(i);
        osg::ref_ptr<osg::Vec4Array> shGreen = g.getShGreen(i);
        osg::ref_ptr<osg::Vec4Array> shBlue = g.getShBlue(i);
        arrays.push_back(shRed.get());
        arrays.push_back(shGreen.get());
        arrays.push_back(shBlue.get());
        writeArray(os, ("ShRed" + std::to_string(i)).c_str(), shRed.get());
        writeArray(os, ("ShGreen" + std::to_string(i)).c_str(), shGreen.get());
        writeArray(os, ("ShBlue" + std::to_string(i)).c_str(), shBlue.get());
    }
    return true;
}

REGISTER_OBJECT_WRAPPER(GaussianGeometry,
                        new osgVerse::GaussianGeometry,
                        osgVerse::GaussianGeometry,
                        "osg::Object osg::Node osg::Drawable osg::Geometry osgVerse::GaussianGeometry")
{
    ADD_USER_SERIALIZER(GaussianGeometry);
}
