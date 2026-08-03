#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include "InuDevice.h"

class ReaderWriterInudev : public osgDB::ReaderWriter
{
public:
    ReaderWriterInudev()
    {
        supportsExtension("device_inu", "osgVerse pseudo-loader");
    }

    virtual const char* className() const
    {
        return "[osgVerse] INUDEV vision device reader";
    }

    /*virtual ReadResult readImage(const std::string& path, const Options* options) const
    {
        std::string ext; std::string fileName = getRealFileName(path, ext);
        std::ifstream in(fileName, std::ios::in | std::ios::binary);
        if (!in) return ReadResult::FILE_NOT_HANDLED;
        
        // TODO
        return NULL;
    }*/

    virtual ReadResult readObject(const std::string& path, const osgDB::Options* options) const
    {
        std::string fileName(path), ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        osg::ref_ptr<osgVerse::InuDevice> inu = new osgVerse::InuDevice;
        if (!inu->connect(options)) return ReadResult::FILE_NOT_FOUND;
        if (!inu->configure(options)) return ReadResult::ERROR_IN_READING_FILE; else return inu.get();
    }

protected:
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(device_inu, ReaderWriterInudev)
