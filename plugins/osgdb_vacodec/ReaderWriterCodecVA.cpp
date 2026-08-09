#include <osg/io_utils>
#include <osg/UserDataContainer>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/ImageStream>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/Archive>

#include "pipeline/ExternalTexture2D.h"
#include "VaapiDecoder.h"

class ReaderWriterCodecVA : public osgDB::ReaderWriter
{
public:
ReaderWriterCodecVA()
    {
        supportsExtension("codec_va", "Pseudo file extension, used to select the plugin.");
    }

    virtual ~ReaderWriterCodecVA()
    {
    }

    virtual const char* className() const
    { return "[osgVerse] Video codec plugin depending on libVA SDK"; }

    virtual ReadResult readObject(const std::string& path, const osgDB::Options* options = NULL) const
    {
        std::string fileName(path);
        std::string ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        const void* context = (options ? options->getPluginData("Context") : NULL);
        if (context != NULL)
        {
            osg::ref_ptr<osgVerse::GpuResourceReaderWriterContainer> container =
                new osgVerse::GpuResourceReaderWriterContainer;
            std::transform(fileName.begin(), fileName.end(), fileName.begin(), tolower);
            if (fileName.find("encode") != std::string::npos)
            {
                OSG_FATAL << "[ReaderWriterCodecVA] libVA Encoder dependency not found" << std::endl;
                return ReadResult::ERROR_IN_READING_FILE;
            }
            else
            {
                OSG_FATAL << "[ReaderWriterCodecVA] libVA Decoder dependency not found" << std::endl;
                return ReadResult::ERROR_IN_READING_FILE;
            }
            return container.get();
        }
        return ReadResult::FILE_NOT_FOUND;
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(codec_va, ReaderWriterCodecVA)
