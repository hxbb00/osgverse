#undef OBJECT_CAST
#define OBJECT_CAST dynamic_cast

#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/Global.h>
using namespace osgVerse;

namespace wrap_osgVerseStringObject
{
    static bool checkValues(const osgVerse::StringObject& obj) { return true; }
    static bool readValues(osgDB::InputStream& is, osgVerse::StringObject& obj)
    {
        unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
        obj.values.resize(size);
        for (unsigned int i = 0; i < size; ++i) is.readWrappedString(obj.values[i]);
        is >> is.END_BRACKET;
        return true;
    }

    static bool writeValues(osgDB::OutputStream& os, const osgVerse::StringObject& obj)
    {
        os.writeSize(obj.values.size()); os << os.BEGIN_BRACKET << std::endl;
        for (size_t i = 0; i < obj.values.size(); ++i)
        { os.writeWrappedString(obj.values[i]); os << std::endl; }
        os << os.END_BRACKET << std::endl;
        return true;
    }

    // The name token only names the generated C symbols: it is prefixed so that it does not
    // clash with osgEarth's own 'wrapper_serializer_StringObject' when both libraries are linked
    // statically (WASM builds). The registered class name below is unchanged.
    REGISTER_OBJECT_WRAPPER(OSGVERSE_StringObject,
                            new osgVerse::StringObject,
                            osgVerse::StringObject,
                            "osg::Object osgVerse::StringObject")
    {
        ADD_USER_SERIALIZER(Values);
    }
}

namespace wrap_osgVerseUserCallback
{
#if OSG_VERSION_GREATER_THAN(3, 2, 3)
    REGISTER_OBJECT_WRAPPER(UserCallback,
                            new osgVerse::UserCallback,
                            osgVerse::UserCallback,
                            "osg::Object osg::Callback osg::CallbackObject osgVerse::UserCallback")
    {
        // Script callback: only the object name is persisted, the run() logic is provided
        // by user scripts after loading
    }
#else
    REGISTER_OBJECT_WRAPPER(UserCallback,
                            new osgVerse::UserCallback,
                            osgVerse::UserCallback,
                            "osg::Object osgVerse::UserCallback")
    {
    }
#endif
}

namespace wrap_osgVerseCameraDrawCallback
{
    REGISTER_OBJECT_WRAPPER(CameraDrawCallback,
                            new osgVerse::CameraDrawCallback,
                            osgVerse::CameraDrawCallback,
                            "osg::Object osg::Callback osg::Camera::DrawCallback osgVerse::CameraDrawCallback")
    {
        // getSubCallback() has both const and non-const overloads, so the getter is
        // explicitly selected here for osgDB::ObjectSerializer
        wrapper->addSerializer(new osgDB::ObjectSerializer<osgVerse::CameraDrawCallback,
                    osgVerse::CameraDrawCallback>("SubCallback", NULL,
                    static_cast<const osgVerse::CameraDrawCallback*
                        (osgVerse::CameraDrawCallback::*)() const>(&osgVerse::CameraDrawCallback::getSubCallback),
                    &osgVerse::CameraDrawCallback::setSubCallback),
                    osgDB::BaseSerializer::RW_OBJECT);
    }
}

namespace wrap_osgVerseComponentCallback
{
#if OSG_VERSION_GREATER_THAN(3, 2, 3)
    REGISTER_OBJECT_WRAPPER(ComponentCallback,
                            new osgVerse::ComponentCallback,
                            osgVerse::ComponentCallback,
                            "osg::Object osg::Callback osgVerse::ComponentCallback")
    {
        // Components are abstract osg::Object instances whose run() logic is usually
        // implemented by scripts, so the container itself is persisted without them
    }
#endif
}

#undef OBJECT_CAST
#define OBJECT_CAST static_cast
