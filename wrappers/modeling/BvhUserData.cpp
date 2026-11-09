#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <modeling/GaussianGeometry.h>
using namespace osgVerse;

static bool checkBvhData(const BvhUserData& data) { return true; }

static bool readBvhData(osgDB::InputStream& is, BvhUserData& data)
{
    unsigned int dataSize = 0; is >> dataSize;
    std::vector<uint8_t> rawData;
    if (dataSize > 0)
    {
        rawData.resize(dataSize);
        is.readCharArray((char*)&rawData[0], dataSize);
    }
    data.setRawData(rawData);
    return true;
}

static bool writeBvhData(osgDB::OutputStream& os, const BvhUserData& data)
{
    const std::vector<uint8_t>& rawData = data.getRawData();
    unsigned int dataSize = (unsigned int)rawData.size();
    os << dataSize; if (dataSize == 0) return true;
    os.writeCharArray((const char*)&rawData[0], dataSize);
    return true;
}

REGISTER_OBJECT_WRAPPER(BvhUserData,
                        new osgVerse::BvhUserData,
                        osgVerse::BvhUserData,
                        "osg::Object osgVerse::BvhUserData")
{
    ADD_USER_SERIALIZER(BvhData);
}
