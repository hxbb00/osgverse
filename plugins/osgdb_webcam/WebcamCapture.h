#ifndef OSGVERSE_PLUGIN_WEBCAM_CAPTURE_H
#define OSGVERSE_PLUGIN_WEBCAM_CAPTURE_H

#include <string>

namespace osgVerse
{
    /// A captured frame delivered by WebcamCapture.
    /// The implementation always delivers a simple 8-bit image:
    /// channels == 1 (Gray8) or channels == 3 (RGB8, byte order R,G,B).
    struct WebcamFrame
    {
        int width = 0;              ///< pixels per row
        int height = 0;             ///< number of rows
        int stride = 0;             ///< bytes per row, may include padding
        int channels = 3;           ///< 1 = Gray8, 3 = RGB8
        const unsigned char* data = nullptr;

        WebcamFrame() = default;
    };

    /// Platform specific webcam capture backend.
    /// - Windows: Media Foundation (WebcamCaptureMF.cpp)
    /// - Linux:   Video4Linux2  (WebcamCaptureV4L2.cpp)
    /// Other platforms are not supported yet and WebcamCapture::create()
    /// simply returns NULL there.
    class WebcamCapture
    {
    public:
        virtual ~WebcamCapture() {}

        /// Open the device. When \p deviceName is not empty it is used as a
        /// fuzzy name filter and \p deviceIndex is ignored.
        /// \param pixelFormatHint 0 = auto, 1 = prefer RGB8, 2 = prefer Gray8
        virtual bool open(int deviceIndex, const std::string& deviceName,
                          int width, int height, float fps, int pixelFormatHint) = 0;
        virtual void close() = 0;

        virtual std::string deviceName() const = 0;
        virtual int width() const = 0;      ///< actual capture width
        virtual int height() const = 0;     ///< actual capture height

        /// Block until a frame is ready. Returns false on error or when
        /// requestStop() has been called.
        virtual bool readFrame(WebcamFrame& frame) = 0;
        /// Ask a blocking readFrame() to return as soon as possible.
        virtual void requestStop() = 0;

        /// Create the backend for the current platform (NULL if unsupported).
        static WebcamCapture* create();
    };
}

#endif
