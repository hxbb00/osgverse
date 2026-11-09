#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/Drawer2D.h>

namespace wrap_osgVerseDrawer2D
{
    // A standalone osg::Image object does not serialize its pixels through the base
    // Image wrapper (OSG only writes them from osg::Texture), so the pixel payload is
    // stored here to keep a Drawer2D fully reproducible
    static bool checkImageData(const osgVerse::Drawer2D& img) { return true; }

    static bool readImageData(osgDB::InputStream& is, osgVerse::Drawer2D& img)
    {
        int origin = 0, s = 0, t = 0, r = 0, internalFormat = 0;
        int pixelFormat = 0, dataType = 0, packing = 0;
        is >> is.PROPERTY("Layout") >> origin >> s >> t >> r >> internalFormat;
        is >> is.PROPERTY("Format") >> pixelFormat >> dataType >> packing;

        unsigned int size = 0;
        is >> is.PROPERTY("DataSize") >> size;
        if (size > 0)
        {
            unsigned char* data = new unsigned char[size];
            is.readCharArray((char*)data, size);
            img.setImage(s, t, r, internalFormat, pixelFormat, dataType,
                         data, osg::Image::USE_NEW_DELETE, packing);
            img.setOrigin((osg::Image::Origin)origin);
        }
        return true;
    }

    static bool writeImageData(osgDB::OutputStream& os, const osgVerse::Drawer2D& img)
    {
        unsigned int size = (unsigned int)img.getTotalSizeInBytes();
        os << os.PROPERTY("Layout") << (int)img.getOrigin() << img.s() << img.t() << img.r()
           << (int)img.getInternalTextureFormat() << std::endl;
        os << os.PROPERTY("Format") << (int)img.getPixelFormat() << (int)img.getDataType()
           << img.getPacking() << std::endl;
        os << os.PROPERTY("DataSize") << size << std::endl;
        if (size > 0) os.writeCharArray((const char*)img.data(), size);
        return true;
    }

    REGISTER_OBJECT_WRAPPER(Drawer2D,
                            new osgVerse::Drawer2D,
                            osgVerse::Drawer2D,
                            "osg::Object osg::BufferData osg::Image osgVerse::Drawer2D")
    {
        {
            UPDATE_TO_VERSION_SCOPED(154)
                ADDED_ASSOCIATE("osg::BufferData")
        }
        ADD_USER_SERIALIZER(ImageData);
    }
}
