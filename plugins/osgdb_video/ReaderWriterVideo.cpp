#include <osg/io_utils>
#include <osg/UserDataContainer>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/ImageStream>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/Archive>

extern "C"
{
    #define MINIMP4_IMPLEMENTATION
    #include <minimp4.h>
}
#include "pipeline/ExternalTexture2D.h"
#include "readerwriter/Utilities.h"

namespace osgVerse
{
    class MP4VideoDemuxer : public osgVerse::GpuResourceReaderBase::Demuxer
    {
    public:
        struct InputBuffer { uint8_t* buffer; size_t size; };

        MP4VideoDemuxer(const std::string& fileName)
        :   _videoTrack(NULL), _audioTrack(NULL), _currentOffset(0), _currentFrameBytes(0),
            _timestamp(0), _videoNum(0), _audioNum(0), _videoCodec(0), _samplePos(0),
            _width(0), _height(0), _fps(25.0)
        {
            _fileBuffer = loadFileData(fileName);
            if (_fileBuffer.empty()) { _demuxer = NULL; return; }
            else _demuxer = new MP4D_demux_t {};
            
            InputBuffer buffer { _fileBuffer.data(), _fileBuffer.size() };
            if (!MP4D_open(_demuxer, MP4VideoDemuxer::readCallback, &buffer, _fileBuffer.size()))
                { OSG_WARN << "[MP4VideoDemuxer] Failed to open " << fileName << std::endl; }
            else
            {
                for (int i = 0; i < _demuxer->track_count; ++i)
                {
                    MP4D_track_t* track = _demuxer->track + i; if (!track) continue;
                    if (track->handler_type == MP4D_HANDLER_TYPE_VIDE)
                        { _videoTrack = track; _videoNum = i; }
                    else if (track->handler_type == MP4D_HANDLER_TYPE_SOUN)
                        { _audioTrack = track; _audioNum = i; }
                }
                if (_videoTrack != NULL) initVideoTrack();
                if (_audioTrack != NULL) initAudioTrack();
            }
        }

        static int readCallback(int64_t offset, void* buffer, size_t size, void* token)
        {
            InputBuffer* buf = (InputBuffer*)token;
            size_t to_copy = MINIMP4_MIN(size, (buf->size - offset - size));
            memcpy(buffer, buf->buffer + offset, to_copy);
            return to_copy != size;
        }

        virtual osgVerse::VideoCodecType getVideoCodec()
        {
            switch (_videoCodec)
            {
            case MP4_OBJECT_TYPE_AVC: return osgVerse::CODEC_H264;
            case MP4_OBJECT_TYPE_HEVC: return osgVerse::CODEC_HEVC;
            default: return osgVerse::CODEC_INVALID;
            }
        }

        virtual int getWidth() const { return _width; }
        virtual int getHeight() const { return _height; }
        virtual double getFrameRate() const { return _fps; }

        virtual bool demux(unsigned char** dataData, int* dataBytes, long long* pts)
        {
            if (_videoTrack != NULL && _samplePos < _videoTrack->sample_count)
            {
                if (_currentFrameBytes == 0)
                {
                    unsigned int frameBytes = 0, ts = 0, duration = 0;
                    MP4D_file_offset_t ofs = MP4D_frame_offset(
                        _demuxer, _videoNum, _samplePos, &frameBytes, &ts, &duration);
                    _currentOffset = ofs; _currentFrameBytes = frameBytes;
                    _timestamp = ts; _samplePos++;
                }
                
                unsigned char* buf = _fileBuffer.data() + _currentOffset;
                if (_currentFrameBytes > 0)
                {
                    uint32_t size = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                                    | ((uint32_t)buf[2] << 8) | buf[3]; size += 4;
                    _current.resize(size); memcpy(_current.data(), buf, size);

                    unsigned char* bufOut = _current.data();
                    bufOut[0] = 0; bufOut[1] = 0; bufOut[2] = 0; bufOut[3] = 1;
                    *dataData = bufOut; *dataBytes = size; _currentOffset += size;
                    if (_currentFrameBytes < size) _currentFrameBytes = 0;
                    else _currentFrameBytes -= size;
                }
                if (pts) *pts = _timestamp; return true;
            }
            return false;
        }

    protected:
        virtual ~MP4VideoDemuxer()
        {
            if (_demuxer != NULL)
            { MP4D_close(_demuxer); delete _demuxer; }
        }

        void initVideoTrack()
        {
            _videoCodec = _videoTrack->object_type_indication;
            _width = _videoTrack->SampleDescription.video.width;
            _height = _videoTrack->SampleDescription.video.height;
        }

        void initAudioTrack()
        {
            // TODO
        }

        std::vector<unsigned char> _fileBuffer, _current;
        MP4D_demux_t* _demuxer;
        MP4D_track_t* _videoTrack;
        MP4D_track_t* _audioTrack;
        uint64_t _currentOffset, _currentFrameBytes, _timestamp;
        unsigned int _videoNum, _audioNum;
        unsigned int _videoCodec;
        unsigned int _samplePos;
        int _width, _height;
        double _fps;
    };
}

class ReaderWriterVideo : public osgDB::ReaderWriter
{
public:
    ReaderWriterVideo()
    {
        supportsExtension("verse_video", "Pseudo file extension, used to select the plugin.");
        supportsExtension("mp4", "MPEG-4");
        supportsExtension("m4v", "MPEG-4");
    }

    virtual ~ReaderWriterVideo()
    {
    }

    virtual const char* className() const
    { return "[osgVerse] Common video format demuxer plugin"; }

    virtual ReadResult readObject(const std::string& file, const osgDB::Options* options = NULL) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;
        if (ext == "video" || ext == "verse_video")
            return readObject(osgDB::getNameLessExtension(file), options);

        osg::ref_ptr<osgVerse::MP4VideoDemuxer> demuxer = new osgVerse::MP4VideoDemuxer(file);
        if (demuxer->getWidth() > 0 && demuxer->getHeight() > 0)
        {
            osgVerse::GpuResourceDemuxerMuxerContainer* container =
                new osgVerse::GpuResourceDemuxerMuxerContainer;
            container->setDemuxer(demuxer.get()); return container;
        }
        return ReadResult::ERROR_IN_READING_FILE;
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse_video, ReaderWriterVideo)
