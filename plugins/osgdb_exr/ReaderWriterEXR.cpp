#include <osg/Image>
#include <osg/GL>
#include <osg/Notify>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

// Reuse the miniz implementation compiled into osgVerseDependency (rlottie zip.cpp) instead
// of compiling a second copy, which produces duplicate mz_* symbols in static/WASM builds.
#define MINIZ_HEADER_FILE_ONLY
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

namespace
{
    bool getBoolOption(const osgDB::ReaderWriter::Options* options, const char* name, bool def)
    {
        if (!options) return def;
        std::string value = options->getPluginStringData(name);
        if (value.empty()) return def;
        return std::atoi(value.c_str()) != 0;
    }

    GLint getRGBAFloatInternalFormat()
    {
#if defined(OSG_GLES2_AVAILABLE)
        return GL_RGBA;
#elif defined(OSG_GLES3_AVAILABLE)
        return GL_RGBA32F;
#else
        return GL_RGBA32F_ARB;
#endif
    }

    void copyRowsFlipped(unsigned char* dst, const unsigned char* src,
                         int width, int height, size_t rowBytes)
    {
        for (int y = 0; y < height; ++y)
            std::memcpy(dst + (size_t)(height - 1 - y) * rowBytes,
                        src + (size_t)y * rowBytes, rowBytes);
    }
}

class ReaderWriterEXR : public osgDB::ReaderWriter
{
public:
    ReaderWriterEXR()
    {
        supportsOption("FlipVertical", "Flip the image vertical when reading/writing. Default: 1");
        supportsOption("SaveAsHalf", "Save the image as fp16 (HALF) format. Default: 0");
        supportsOption("Layer", "Layer name to read from a multi-layer EXR file");
        supportsExtension("verse_exr", "osgVerse pseudo-loader");
        supportsExtension("exr", "OpenEXR image file");
    }

    virtual const char* className() const { return "[osgVerse] OpenEXR image reader"; }

    virtual ReadResult readObject(std::istream& fin, const Options* options = NULL) const
    { return readImage(fin, options); }

    virtual ReadResult readObject(const std::string& file, const Options* options = NULL) const
    { return readImage(file, options); }

    virtual ReadResult readImage(std::istream& fin, const Options* options = NULL) const
    {
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(fin)),
                                        std::istreambuf_iterator<char>());
        if (data.empty()) return ReadResult::ERROR_IN_READING_FILE;

        float* rgba = NULL; int w = 0, h = 0; const char* err = NULL;
        int result = LoadEXRFromMemory(&rgba, &w, &h, data.data(), data.size(), &err);
        return finishReading(rgba, w, h, err, result, std::string(), options);
    }

    virtual ReadResult readImage(const std::string& path, const Options* options = NULL) const
    {
        std::string ext, fileName = getRealFileName(path, ext, false);
        if (fileName.empty()) return ReadResult::FILE_NOT_HANDLED;

        float* rgba = NULL; int w = 0, h = 0; const char* err = NULL; int result = 0;
        std::string layer = options ? options->getPluginStringData("Layer") : std::string();
        if (layer.empty())
            result = LoadEXR(&rgba, &w, &h, fileName.c_str(), &err);
        else
            result = LoadEXRWithLayer(&rgba, &w, &h, fileName.c_str(), layer.c_str(), &err);
        return finishReading(rgba, w, h, err, result, path, options);
    }

    virtual WriteResult writeImage(const osg::Image& image, const std::string& path,
                                   const Options* options = NULL) const
    {
        std::string ext, fileName = getRealFileName(path, ext, true);
        if (fileName.empty()) return WriteResult::FILE_NOT_HANDLED;

        const int w = image.s(), h = image.t();
        const int comps = image.computeNumComponents(image.getPixelFormat());
        GLenum dataType = image.getDataType();
        if (w < 1 || h < 1 || image.r() != 1) return WriteResult::ERROR_IN_WRITING_FILE;
        if (comps != 3 && comps != 4) return WriteResult::ERROR_IN_WRITING_FILE;
        if (dataType != GL_FLOAT && dataType != GL_HALF_FLOAT)
            return WriteResult::ERROR_IN_WRITING_FILE;

        const bool flip = getBoolOption(options, "FlipVertical", true);
        const bool saveHalf = getBoolOption(options, "SaveAsHalf", false);
        const size_t elementSize = (dataType == GL_FLOAT) ? 4 : 2;
        const size_t srcRow = (size_t)w * comps * elementSize;
        std::vector<float> pixels((size_t)w * h * comps);
        for (int y = 0; y < h; ++y)
        {
            int srcY = flip ? (h - 1 - y) : y;
            const unsigned char* src = image.data() + srcY * srcRow;
            float* dst = pixels.data() + (size_t)y * w * comps;
            if (dataType == GL_FLOAT)
                std::memcpy(dst, src, (size_t)w * comps * 4);
            else
            {
                const unsigned short* halfData = (const unsigned short*)src;
                for (int i = 0; i < w * comps; ++i)
                {
                    tinyexr::FP16 h16; h16.u = halfData[i];
                    dst[i] = tinyexr::half_to_float(h16).f;
                }
            }
        }

        const char* err = NULL;
        if (SaveEXR(pixels.data(), w, h, comps, saveHalf ? 1 : 0, fileName.c_str(), &err)
            != TINYEXR_SUCCESS)
        {
            if (err) { OSG_WARN << "[osgVerse] EXR write error: " << err << std::endl;
                       FreeEXRErrorMessage(err); }
            return WriteResult::ERROR_IN_WRITING_FILE;
        }
        return WriteResult::FILE_SAVED;
    }

    virtual WriteResult writeImage(const osg::Image&, std::ostream&,
                                   const Options* = NULL) const
    { return WriteResult::FILE_NOT_HANDLED; }

protected:
    ReadResult finishReading(float* rgba, int w, int h, const char* err, int result,
                             const std::string& fileName, const Options* options) const
    {
        if (result != TINYEXR_SUCCESS || !rgba || w < 1 || h < 1)
        {
            if (err) { OSG_WARN << "[osgVerse] EXR read error: " << err << std::endl;
                       FreeEXRErrorMessage(err); }
            if (rgba) free(rgba);
            return ReadResult::ERROR_IN_READING_FILE;
        }

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(w, h, 1, GL_RGBA, GL_FLOAT);
        image->setInternalTextureFormat(getRGBAFloatInternalFormat());
        const size_t rowBytes = (size_t)w * 4 * sizeof(float);
        if (getBoolOption(options, "FlipVertical", true))
            copyRowsFlipped(image->data(), (const unsigned char*)rgba, w, h, rowBytes);
        else
            std::memcpy(image->data(), rgba, rowBytes * h);
        free(rgba);

        if (!fileName.empty()) image->setFileName(fileName);
        return image.release();
    }

    std::string getRealFileName(const std::string& path, std::string& ext, bool forWrite) const
    {
        ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return std::string();
        if (ext == "verse_exr")
        {
            std::string fileName = osgDB::getNameLessExtension(path);
            ext = osgDB::getLowerCaseFileExtension(fileName);
            return forWrite ? fileName : osgDB::findDataFile(fileName);
        }
        return forWrite ? path : osgDB::findDataFile(path);
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse_exr, ReaderWriterEXR)
