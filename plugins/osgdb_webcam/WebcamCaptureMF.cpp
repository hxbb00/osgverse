// Windows Media Foundation webcam capture backend.
// Enumeration:  MFEnumDeviceSources (MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP)
// Streaming:    IMFSourceReader with a RGB32 output media type (the built-in
//               video processor converts NV12/MJPG/... to RGB32 automatically).
// The captured RGB32 (BGRA in memory) frames are converted to RGB8 and handed
// to the platform independent WebcamDevice.

#ifdef _WIN32

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "WebcamCapture.h"
#include <osg/Notify>

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace osgVerse
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        std::string utf8FromWide(const wchar_t* wstr)
        {
            if (!wstr) return "";
            int len = ::WideCharToMultiByte(
                CP_UTF8, 0, wstr, -1, NULL,
                0, NULL, NULL);
            if (len <= 1) return "";
            std::string out((size_t)len - 1, '\0');
            ::WideCharToMultiByte(
                CP_UTF8, 0, wstr, -1, &out[0],
                len, NULL, NULL);
            return out;
        }

        void ensureComInitialized()
        {
            static thread_local bool comInitialized = false;
            if (!comInitialized)
            {
                ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
                comInitialized = true;
            }
        }
    }

    class MFWebcamCapture : public WebcamCapture
    {
    public:
        MFWebcamCapture() = default;
        virtual ~MFWebcamCapture() { close(); }

        virtual bool open(int deviceIndex, const std::string& deviceName,
                          int width, int height, float fps, int pixelFormatHint)
        {
            close(); _stopRequested = false;
            ensureComInitialized();
            if (!ensureMFStartup()) return false;
            _mfStarted = true;

            // Enumerate video capture devices
            ComPtr<IMFAttributes> attrs;
            if (FAILED(::MFCreateAttributes(&attrs, 2))) { close(); return false; }
            if (FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
            { close(); return false; }

            IMFActivate** ppDevices = NULL;
            UINT32 count = 0;
            HRESULT hr = ::MFEnumDeviceSources(attrs.Get(), &ppDevices, &count);
            if (FAILED(hr) || count == 0)
            {
                OSG_WARN << "[WebcamCaptureMF] No video capture device found." << std::endl;
                close();
                return false;
            }

            // Pick the device: fuzzy name match first, otherwise by index
            UINT32 selected = (UINT32)deviceIndex;
            bool selectedByIndex = deviceName.empty();
            if (!deviceName.empty())
            {
                bool found = false;
                for (UINT32 i = 0; i < count; ++i)
                {
                    LPWSTR name = NULL; UINT32 nameLen = 0;
                    if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
                    {
                        std::string n = utf8FromWide(name);
                        ::CoTaskMemFree(name);
                        if (n.find(deviceName) != std::string::npos)
                        { selected = i; found = true; break; }
                    }
                }

                if (!found)
                {
                    OSG_WARN << "[WebcamCaptureMF] No device matching \"" << deviceName << "\"." << std::endl;
                    for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
                    ::CoTaskMemFree(ppDevices);
                    close(); return false;
                }
            }
            else if (selected >= count)
            {
                OSG_WARN << "[WebcamCaptureMF] Device index " << deviceIndex
                         << " out of range (count = " << count << ")." << std::endl;
                for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
                ::CoTaskMemFree(ppDevices);
                close(); return false;
            }

            ComPtr<IMFActivate> activate;
            activate.Attach(ppDevices[selected]);
            for (UINT32 i = 0; i < count; ++i)
                { if (i != selected && ppDevices[i]) ppDevices[i]->Release(); }
            ::CoTaskMemFree(ppDevices);

            // Friendly name
            LPWSTR friendlyName = NULL; UINT32 friendlyLen = 0;
            if (SUCCEEDED(activate->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &friendlyLen)))
            {
                _deviceName = utf8FromWide(friendlyName);
                ::CoTaskMemFree(friendlyName);
            }
            else
            {
                _deviceName = selectedByIndex
                    ? ("Webcam #" + std::to_string(selected))
                    : ("Webcam [" + deviceName + "]");
            }

            // Activate the media source
            hr = activate->ActivateObject(IID_PPV_ARGS(_mediaSource.GetAddressOf()));
            if (FAILED(hr))
            {
                OSG_WARN << "[WebcamCaptureMF] ActivateObject failed (0x"
                         << std::hex << (unsigned long)hr << std::dec << ")." << std::endl;
                close(); return false;
            }

            // Create the source reader with video processing enabled so that
            // any native format can be converted to RGB32.
            ComPtr<IMFAttributes> readerAttrs;
            if (FAILED(::MFCreateAttributes(&readerAttrs, 1)) ||
                FAILED(readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
                FAILED(::MFCreateSourceReaderFromMediaSource(
                    _mediaSource.Get(), readerAttrs.Get(), _reader.GetAddressOf())))
            {
                OSG_WARN << "[WebcamCaptureMF] Failed to create source reader." << std::endl;
                close(); return false;
            }
            if (!setupReader(width, height, fps)) { close(); return false; }
            return true;
        }

        virtual void close()
        {
            _stopRequested = true;
            if (_reader)
            {
                // Break a pending ReadSample() from another thread.
                _reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
                _reader.Reset();
            }
            if (_mediaSource) { _mediaSource->Shutdown(); _mediaSource.Reset(); }
            if (_mfStarted) { ensureMFShutdown(); _mfStarted = false; }
            _deviceName.clear();
            _rgbBuffer.clear();
        }

        virtual std::string deviceName() const { return _deviceName; }
        virtual int width() const { return _width; }
        virtual int height() const { return _height; }

        virtual bool readFrame(WebcamFrame& frame)
        {
            if (!_reader) return false;
            ensureComInitialized();

            while (!_stopRequested)
            {
                DWORD streamIndex = 0, flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                HRESULT hr = _reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                 &streamIndex, &flags, &timestamp,
                                                 sample.GetAddressOf());
                if (FAILED(hr))
                {
                    OSG_NOTICE << "[WebcamCaptureMF] Failed with ReadSample(): " << GetLastError() << std::endl;
                    if (_stopRequested) break; continue;
                }

                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                {
                    // Camera disconnected / stream ended: try to restart it.
                    OSG_NOTICE << "[WebcamCaptureMF] Webcam disconnected" << std::endl;
                    if (!restartStream()) break; continue;
                }

                if (sample)
                {
                    ComPtr<IMFMediaBuffer> buffer;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())))
                    {
                        BYTE* pData = NULL; DWORD maxLen = 0, curLen = 0;
                        if (SUCCEEDED(buffer->Lock(&pData, &maxLen, &curLen)))
                        {
                            if (pData && curLen >= (DWORD)_width * (DWORD)_height * 4)
                            {
                                // RGB32 (BGRA in memory) -> RGB8
                                _rgbBuffer.resize((size_t)_width * _height * 3);
                                const BYTE* src = pData;
                                unsigned char* dst = _rgbBuffer.data();
                                for (int i = 0; i < _width * _height; ++i, src += 4, dst += 3)
                                    { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; }
                                buffer->Unlock();

                                frame.width = _width; frame.height = _height;
                                frame.stride = _width * 3; frame.channels = 3;
                                frame.data = _rgbBuffer.data();
                                return true;
                            }
                            buffer->Unlock();
                        }
                    }
                    else
                    {
                        OSG_NOTICE << "[WebcamCaptureMF] Failed with ConvertToContiguousBuffer(): "
                                   << GetLastError() << std::endl;
                    }
                }
            }
            return false;
        }

        virtual void requestStop()
        {
            _stopRequested = true;
            if (_reader)
                _reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
        }

    private:
        static bool ensureMFStartup()
        {
            if (::InterlockedIncrement(&s_mfInitCount) == 1)
                s_mfInitResult = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
            return SUCCEEDED(s_mfInitResult);
        }

        static void ensureMFShutdown()
        {
            if (::InterlockedDecrement(&s_mfInitCount) == 0)
            {
                if (SUCCEEDED(s_mfInitResult)) ::MFShutdown();
                s_mfInitResult = E_FAIL;
            }
        }

        bool restartStream()
        {
            if (FAILED(_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE)))
                return false;
            if (FAILED(_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, NULL)))
                return false;
            if (FAILED(_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)))
                return false;
            return true;
        }

        bool setupReader(int width, int height, float fps)
        {
            // Query the first native video type for default size / frame rate.
            UINT32 nativeW = 0, nativeH = 0, nativeFpsN = 0, nativeFpsD = 1;
            for (DWORD i = 0; ; ++i)
            {
                ComPtr<IMFMediaType> mt;
                if (FAILED(_reader->GetNativeMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, mt.GetAddressOf()))) break;
                GUID major = GUID_NULL;
                if (FAILED(mt->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;
                if (major != MFMediaType_Video) continue;

                UINT32 w = 0, h = 0;
                if (SUCCEEDED(::MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h)
                { nativeW = w; nativeH = h; }
                UINT32 n = 0, d = 0;
                if (SUCCEEDED(::MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &n, &d)) && n && d)
                { nativeFpsN = n; nativeFpsD = d; }
                break;   // only the first video media type is needed
            }

            UINT32 outW = (width > 0) ? (UINT32)width : nativeW;
            UINT32 outH = (height > 0) ? (UINT32)height : nativeH;
            if (!outW || !outH) { outW = 640; outH = 480; }

            UINT32 fpsN = nativeFpsN, fpsD = nativeFpsD;
            if (fps > 0.0f) { fpsN = (UINT32)(fps * 1000.0f); fpsD = 1000; }

            ComPtr<IMFMediaType> outType;
            if (FAILED(::MFCreateMediaType(&outType))) return false;
            if (FAILED(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return false;
            if (FAILED(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))) return false;
            if (FAILED(outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) return false;
            if (FAILED(::MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, outW, outH))) return false;
            if (FAILED(::MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fpsN, fpsD))) return false;

            HRESULT hr = _reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outType.Get());
            if (FAILED(hr))
            {
                // Fallback: only force RGB32, leave size / rate untouched.
                ComPtr<IMFMediaType> fallback;
                if (FAILED(::MFCreateMediaType(&fallback))) return false;
                if (FAILED(fallback->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return false;
                if (FAILED(fallback->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))) return false;
                if (FAILED(fallback->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) return false;
                hr = _reader->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, fallback.Get());
                if (FAILED(hr)) return false;
            }

            // Query the actual output size.
            ComPtr<IMFMediaType> curType;
            if (SUCCEEDED(_reader->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, curType.GetAddressOf())))
            {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(::MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h)
                { _width = (int)w; _height = (int)h; }
            }
            if (_width <= 0 || _height <= 0) { _width = (int)outW; _height = (int)outH; }
            return true;
        }

        static LONG s_mfInitCount;
        static HRESULT s_mfInitResult;

        ComPtr<IMFMediaSource> _mediaSource;
        ComPtr<IMFSourceReader> _reader;
        std::string _deviceName;
        int _width = 0, _height = 0;

        std::vector<unsigned char> _rgbBuffer;
        bool _stopRequested = false;
        bool _mfStarted = false;
    };

    LONG MFWebcamCapture::s_mfInitCount = 0;
    HRESULT MFWebcamCapture::s_mfInitResult = E_FAIL;

    WebcamCapture* WebcamCapture::create()
    { return new MFWebcamCapture; }
}

#endif  // _WIN32
