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

namespace osgVerse
{
    /** To use libVA under Linux with NVIDIA card:
        - sudo apt-get install nvidia-vaapi-driver
     */
    class VaapiResourceReader : public osgVerse::GpuResourceReaderBase
    {
    public:
        VaapiResourceReader(void* dis) : osgVerse::GpuResourceReaderBase(dis) {}
        virtual ~VaapiResourceReader() {}

        virtual bool openResource(Demuxer* demuxer)
        {
            _demuxer = demuxer; if (!demuxer) return false;
            if (getResourceType() == RES_EGL)
            {
                if (_demuxer->getVideoCodec() != osgVerse::CODEC_INVALID)
                {
                    _width = (_demuxer->getWidth() + 1) & ~1; _height = _demuxer->getHeight();
                    _decoder = new VaapiDecoder((int)_demuxer->getVideoCodec(), _width, _height);
                    if (!_decoder->initialize()) return false;
                    //if (_testImage.valid() && _testImage->valid()) _testImage->scaleImage(_width, _height, 1);
                }
            }
            return true;
        }

        virtual void releaseGpu()
        {
            if (getResourceType() == RES_EGL) { _demuxer = NULL; _decoder = NULL; }
            osgVerse::GpuResourceReaderBase::releaseGpu();
        }

        virtual void operator()(osg::StateAttribute* sa, osg::NodeVisitor* nv)
        {
            if (_demuxer && !_decoder)
            {
                if (getResourceType() == RES_EGL)
                {
                    if (_demuxer->getVideoCodec() == osgVerse::CODEC_INVALID) return;
                    _width = (_demuxer->getWidth() + 1) & ~1; _height = _demuxer->getHeight();
                    _decoder = new VaapiDecoder((int)_demuxer->getVideoCodec(), _width, _height);
                    if (!_decoder->initialize()) return;
                }
            }

            uint8_t *video = NULL, *frame = NULL; int videoBytes = 0; long long pts = 0;
            if (!_demuxer || !_decoder) { setState(INVALID); return; }
            else if (!_decoder->isInitialized()) { setState(INVALID); return; }
            
            if (!_demuxer->demux(&video, &videoBytes, &pts)) { setState(PENDING); return; }
            if (!_decoder->decode(video, videoBytes, pts)) { setState(PENDING); return; }

            if (_decoder->convert() && getResourceType() == RES_EGL)
            {
                EglResourceHandle* H = static_cast<EglResourceHandle*>(_handle.get());
                if (H->image == NULL)
                {
                    std::vector<VaapiDecoder::ExportedBufferData> drmData; int fourcc = 0;
                    if (_decoder->exportDecodedFrame(fourcc, drmData))
                    {
                        std::vector<int> desc(6);  // as converted to RGB32, we expect drmData to have only 1 layer
                        const VaapiDecoder::ExportedBufferData& eBuf = drmData[0];
                        desc[0] = eBuf.fd; desc[1] = fourcc; desc[2] = eBuf.drmPrimeOffset; desc[3] = eBuf.drmPrimePitch;
                        desc[4] = (int)(eBuf.drmPrimeModifier & ((((uint64_t)1) << 33) - 1));
                        desc[5] = (int)((eBuf.drmPrimeModifier >> 32) & ((((uint64_t)1) << 33) - 1));
                        getDeviceDescriptor(desc, 1);  // Create the relationship between EGLImage and VASurface
                    }
                }
                setState(PLAYING);
            }
            else setState(STOPPED);
        }

    protected:
        osg::ref_ptr<VaapiDecoder> _decoder;
    };
}

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
                container->setReader(new osgVerse::VaapiResourceReader((void*)context));
            return container.get();
        }
        return ReadResult::FILE_NOT_FOUND;
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(codec_va, ReaderWriterCodecVA)
