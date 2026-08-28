#include <osg/io_utils>
#include <osg/UserDataContainer>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/ImageStream>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>

#include "pipeline/Global.h"
#include "wrappers/Export.h"
#include "readerwriter/Utilities.h"

class ReaderWriterVerse : public osgDB::ReaderWriter
{
public:
    ReaderWriterVerse()
    {
        supportsExtension("verse", "Pseudo file extension, used to select the plugin.");
        supportsExtension("osgverse", "Pseudo file extension, used to select the plugin.");

        osg::ref_ptr<osgDB::Registry> regObject = osgDB::Registry::instance();
#ifndef OSG_LIBRARY_STATIC
        // Load necessary libraries and wrappers
        regObject->loadLibrary(regObject->createLibraryNameForNodeKit("osgVerseReaderWriter"));
        regObject->loadLibrary(regObject->createLibraryNameForNodeKit("osgVerseWrappers"));

        // Pre-load libraries to register web/streaming/database protocols
        regObject->loadLibrary(regObject->createLibraryNameForExtension("verse_web"));
        regObject->loadLibrary(regObject->createLibraryNameForExtension("verse_ms"));
        regObject->loadLibrary(regObject->createLibraryNameForExtension("verse_odbc"));
        regObject->loadLibrary(regObject->createLibraryNameForExtension("verse_leveldb"));
        regObject->loadLibrary(regObject->createLibraryNameForExtension("verse_mbtiles"));
#endif
        osgVerse::updateOsgBinaryWrappers();
    }

    virtual ~ReaderWriterVerse()
    {
    }

    virtual const char* className() const
    { return "[osgVerse] osgVerse pseudo-file plugin to load necessary libraries before loading"; }

    virtual ReadResult readObject(const std::string& file, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;
        return osgDB::readRefObjectFile(osgDB::getNameLessExtension(file), options);
    }

    virtual ReadResult readImage(const std::string& file, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;
        return osgDB::readRefImageFile(osgDB::getNameLessExtension(file), options);
    }

    virtual ReadResult readNode(const std::string& file, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;
        return osgDB::readRefNodeFile(osgDB::getNameLessExtension(file), options);
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse, ReaderWriterVerse)
