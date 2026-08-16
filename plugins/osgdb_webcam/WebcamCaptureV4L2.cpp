// Linux Video4Linux2 webcam capture backend.
// Uses the classic mmap + select() capture flow:
//   VIDIOC_QUERYCAP -> VIDIOC_ENUM_FMT -> VIDIOC_S_FMT -> VIDIOC_S_PARM
//   -> VIDIOC_REQBUFS -> VIDIOC_QUERYBUF -> mmap -> VIDIOC_QBUF
//   -> VIDIOC_STREAMON -> loop { select; VIDIOC_DQBUF; ...; VIDIOC_QBUF }
//   -> VIDIOC_STREAMOFF
// Decodable pixel formats: RGB24 (memcpy), YUYV (converted to RGB8) and
// GREY (single channel). MJPEG is not decodable here and is skipped.

#ifdef __linux__

#include "WebcamCapture.h"
#include <osg/Notify>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/videodev2.h>

#include <atomic>
#include <string>
#include <vector>

namespace osgVerse
{
    namespace
    {
        inline unsigned char clampByte(int v)
        { return (v < 0) ? 0 : ((v > 255) ? 255 : (unsigned char)v); }

        // BT.601 YUV -> RGB, integer approximation.
        inline void yuv2rgb(unsigned char Y, unsigned char U, unsigned char V,
                            unsigned char* rgb)
        {
            int c = (int)Y - 16, d = (int)U - 128, e = (int)V - 128;
            rgb[0] = clampByte((298 * c + 409 * e + 128) >> 8);
            rgb[1] = clampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
            rgb[2] = clampByte((298 * c + 516 * d + 128) >> 8);
        }
    }

    class V4L2WebcamCapture : public WebcamCapture
    {
    public:
        V4L2WebcamCapture() = default;
        virtual ~V4L2WebcamCapture() { close(); }

        virtual bool open(int deviceIndex, const std::string& deviceName,
                          int width, int height, float fps, int pixelFormatHint)
        {
            // Open the device (by name or by index).
            int fd = -1; close(); _stopRequested = false;
            if (!deviceName.empty())
            {
                for (int i = 0; i < 16 && fd < 0; ++i)
                {
                    std::string path = "/dev/video" + std::to_string(i);
                    int f = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
                    if (f < 0) continue;

                    v4l2_capability cap; memset(&cap, 0, sizeof(cap));
                    bool match = ::ioctl(f, VIDIOC_QUERYCAP, &cap) == 0 &&
                                 (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
                                 std::string((const char*)cap.card).find(deviceName) != std::string::npos;
                    if (match) { fd = f; _deviceName = (const char*)cap.card; }
                    else ::close(f);
                }

                if (fd < 0)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] No device matching \"" << deviceName << "\"." << std::endl;
                    return false;
                }
            }
            else
            {
                std::string path = "/dev/video" + std::to_string(deviceIndex);
                fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
                if (fd < 0)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] Cannot open " << path
                             << ": " << strerror(errno) << std::endl;
                    return false;
                }
            }

            _fd = fd;
            if (_deviceName.empty())
            {
                v4l2_capability cap;
                memset(&cap, 0, sizeof(cap));
                if (::ioctl(_fd, VIDIOC_QUERYCAP, &cap) == 0)
                    _deviceName = (const char*)cap.card;
            }

            // Enumerate supported pixel formats.
            bool hasYUYV = false, hasRGB = false, hasGray = false, hasMJPEG = false;
            v4l2_fmtdesc desc; memset(&desc, 0, sizeof(desc));
            desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            for (desc.index = 0; ::ioctl(_fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index)
            {
                if (desc.pixelformat == V4L2_PIX_FMT_YUYV) hasYUYV = true;
                else if (desc.pixelformat == V4L2_PIX_FMT_RGB24) hasRGB = true;
                else if (desc.pixelformat == V4L2_PIX_FMT_GREY) hasGray = true;
                else if (desc.pixelformat == V4L2_PIX_FMT_MJPEG ||
                         desc.pixelformat == V4L2_PIX_FMT_JPEG) hasMJPEG = true;
            }

            // Choose a decodable pixel format.
            __u32 wantFmt = 0;
            if (pixelFormatHint == 2 && hasGray) wantFmt = V4L2_PIX_FMT_GREY;
            else if (pixelFormatHint == 1 && hasRGB) wantFmt = V4L2_PIX_FMT_RGB24;

            if (!wantFmt)
            {
                if (hasRGB) wantFmt = V4L2_PIX_FMT_RGB24;
                else if (hasYUYV) wantFmt = V4L2_PIX_FMT_YUYV;
                else if (hasGray) wantFmt = V4L2_PIX_FMT_GREY;
            }
            if (!wantFmt)
            {
                OSG_WARN << "[WebcamCaptureV4L2] No decodable pixel format on " << _deviceName
                         << " (MJPEG-only cameras are not supported, hasMJPEG = "
                         << (hasMJPEG ? "yes" : "no") << ")." << std::endl;
                close(); return false;
            }

            // Set the format; the driver may adjust the resolution.
            v4l2_format fmt; memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt.fmt.pix.width = (__u32)width;
            fmt.fmt.pix.height = (__u32)height;
            fmt.fmt.pix.pixelformat = wantFmt;
            fmt.fmt.pix.field = V4L2_FIELD_ANY;
            if (::ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0)
            {
                v4l2_format cur; memset(&cur, 0, sizeof(cur));
                cur.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (::ioctl(_fd, VIDIOC_G_FMT, &cur) < 0)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] VIDIOC_S_FMT/VIDIOC_G_FMT failed." << std::endl;
                    close(); return false;
                }
                fmt = cur;
            }
            _width = (int)fmt.fmt.pix.width;
            _height = (int)fmt.fmt.pix.height;
            _pixelFormat = fmt.fmt.pix.pixelformat;
            _bytesPerLine = fmt.fmt.pix.bytesperline;
            if (_bytesPerLine == 0) _bytesPerLine = (unsigned int)_width * pixelSize(_pixelFormat);

            // Request a frame rate (best effort).
            if (fps > 0.0f)
            {
                v4l2_streamparm parm; memset(&parm, 0, sizeof(parm));
                parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                parm.parm.capture.timeperframe.numerator = 1000;
                parm.parm.capture.timeperframe.denominator = (__u32)(fps * 1000.0f);
                if (::ioctl(_fd, VIDIOC_S_PARM, &parm) < 0)
                    OSG_NOTICE << "[WebcamCaptureV4L2] Frame rate request ignored by the driver." << std::endl;
            }

            // Allocate mmap buffers.
            v4l2_requestbuffers req; memset(&req, 0, sizeof(req));
            req.count = 4; req.memory = V4L2_MEMORY_MMAP;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (::ioctl(_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
            {
                OSG_WARN << "[WebcamCaptureV4L2] VIDIOC_REQBUFS failed." << std::endl;
                close(); return false;
            }

            _buffers.resize(req.count);
            for (unsigned int i = 0; i < req.count; ++i)
            {
                v4l2_buffer buf; memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
                if (::ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] VIDIOC_QUERYBUF failed." << std::endl;
                    close(); return false;
                }

                _buffers[i].length = buf.length;
                _buffers[i].start = ::mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, _fd, buf.m.offset);
                if (_buffers[i].start == MAP_FAILED)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] mmap failed." << std::endl;
                    close(); return false;
                }
                if (::ioctl(_fd, VIDIOC_QBUF, &buf) < 0)
                {
                    OSG_WARN << "[WebcamCaptureV4L2] VIDIOC_QBUF failed." << std::endl;
                    close(); return false;
                }
            }

            v4l2_buf_type streamType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (::ioctl(_fd, VIDIOC_STREAMON, &streamType) < 0)
            {
                OSG_WARN << "[WebcamCaptureV4L2] VIDIOC_STREAMON failed." << std::endl;
                close(); return false;
            }
            return true;
        }

        virtual void close()
        {
            _stopRequested = true;
            if (_fd >= 0)
            {
                v4l2_buf_type streamType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                ::ioctl(_fd, VIDIOC_STREAMOFF, &streamType);
                for (auto& b : _buffers)
                {
                    if (b.start && b.start != MAP_FAILED) ::munmap(b.start, b.length);
                    b.start = NULL;
                }
                _buffers.clear();
                ::close(_fd); _fd = -1;
            }
            _deviceName.clear();
            _rgbBuffer.clear();
        }

        virtual std::string deviceName() const { return _deviceName; }
        virtual int width() const { return _width; }
        virtual int height() const { return _height; }

        virtual bool readFrame(WebcamFrame& frame)
        {
            if (_fd < 0) return false;
            while (!_stopRequested)
            {
                fd_set fds; FD_ZERO(&fds); FD_SET(_fd, &fds);
                timeval tv; tv.tv_sec = 0;
                tv.tv_usec = 200000;   // 200 ms poll interval for a quick stop

                int r = ::select(_fd + 1, &fds, NULL, NULL, &tv);
                if (r < 0)
                {
                    if (errno == EINTR) continue;
                    return false;
                }
                if (r == 0) continue;  // timeout; re-check the stop flag

                v4l2_buffer buf; memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                if (::ioctl(_fd, VIDIOC_DQBUF, &buf) < 0)
                {
                    if (errno == EAGAIN) continue;
                    return false;
                }

                const unsigned char* src = (const unsigned char*)_buffers[buf.index].start;
                bool ok = convertFrame(src, buf.bytesused, frame);
                ::ioctl(_fd, VIDIOC_QBUF, &buf); if (ok) return true;
            }
            return false;
        }

        virtual void requestStop() { _stopRequested = true; }

    private:
        static int pixelSize(__u32 fmt)
        {
            switch (fmt)
            {
            case V4L2_PIX_FMT_YUYV:  return 2;
            case V4L2_PIX_FMT_RGB24: return 3;
            case V4L2_PIX_FMT_GREY:  return 1;
            default:                 return 3;
            }
        }

        bool convertFrame(const unsigned char* src, unsigned int bytesUsed, WebcamFrame& out)
        {
            const int w = _width, h = _height;
            if (!src || w <= 0 || h <= 0) return false;

            switch (_pixelFormat)
            {
            case V4L2_PIX_FMT_RGB24:
                {
                    _rgbBuffer.resize((size_t)w * h * 3);
                    unsigned char* dst = _rgbBuffer.data();
                    unsigned int bpl = (_bytesPerLine >= (unsigned int)(w * 3))
                                     ? _bytesPerLine : (unsigned int)(w * 3);
                    for (int y = 0; y < h; ++y)
                        memcpy(dst + (size_t)y * w * 3, src + (size_t)y * bpl, (size_t)w * 3);
                    out.width = w; out.height = h;
                    out.stride = w * 3; out.channels = 3; out.data = dst;
                    return true;
                }
            case V4L2_PIX_FMT_YUYV:
                {
                    if (bytesUsed < (unsigned int)(w * h * 2)) return false;
                    _rgbBuffer.resize((size_t)w * h * 3);

                    unsigned char* dst = _rgbBuffer.data();
                    const unsigned char* sp = src;
                    for (int i = 0; i < w * h / 2; ++i, sp += 4, dst += 6)
                    {
                        yuv2rgb(sp[0], sp[1], sp[3], dst);
                        yuv2rgb(sp[2], sp[1], sp[3], dst + 3);
                    }
                    out.width = w; out.height = h;
                    out.stride = w * 3; out.channels = 3; out.data = _rgbBuffer.data();
                    return true;
                }
            case V4L2_PIX_FMT_GREY:
                {
                    _rgbBuffer.resize((size_t)w * h);
                    unsigned char* dst = _rgbBuffer.data();
                    unsigned int bpl = (_bytesPerLine >= (unsigned int)w)
                                     ? _bytesPerLine : (unsigned int)w;
                    for (int y = 0; y < h; ++y)
                        memcpy(dst + (size_t)y * w, src + (size_t)y * bpl, (size_t)w);
                    out.width = w; out.height = h;
                    out.stride = w; out.channels = 1; out.data = dst;
                    return true;
                }
            default:
                return false;
            }
        }

        struct Buffer
        {
            void* start = NULL;
            size_t length = 0;
        };

        int _fd = -1;
        std::vector<Buffer> _buffers;
        std::string _deviceName;
        int _width = 0, _height = 0;
        __u32 _pixelFormat = 0;
        unsigned int _bytesPerLine = 0;
        std::vector<unsigned char> _rgbBuffer;
        std::atomic<bool> _stopRequested{false};
    };

    WebcamCapture* WebcamCapture::create()
    { return new V4L2WebcamCapture; }
}

#endif  // __linux__
