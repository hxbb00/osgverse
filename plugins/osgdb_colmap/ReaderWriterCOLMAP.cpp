// COLMAP dataset reader for the osgVerse 3DGS trainer.
//
// Usage:
//   osg::ref_ptr<osgVerse::TrainerInput> input =
//       osgDB::readObjectFile("colmap:///path/to/dataset");
//   // or: osgDB::readObjectFile("/path/to/dataset.colmap");
//
// Supported inputs are COLMAP SfM results in binary or text form:
//   sparse/0/{cameras,images,points3D}.{bin,txt}
// plus a directory of images. Options:
//   imageDir=<name or absolute path>  (default: auto-detect images*)
//   maskDir=<name or absolute path>   (optional)
//   evalInterval=<n>                  (default: 8)
//   globalScale=<f>                   (default: 1.0)
#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Options>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <Eigen/Geometry>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <modeling/GaussianTrainer.h>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <sys/stat.h>
#endif

namespace
{

const char* kCameraModelNames[] = {
    "SIMPLE_PINHOLE", "PINHOLE", "SIMPLE_RADIAL", "RADIAL", "OPENCV",
    "OPENCV_FISHEYE", "FULL_OPENCV", "FOV", "SIMPLE_RADIAL_FISHEYE",
    "RADIAL_FISHEYE", "THIN_PRISM_FISHEYE", "RAD_TAN_THIN_PRISM_FISHEYE"
};
const int kCameraModelNumParams[] = { 3, 4, 4, 5, 8, 8, 12, 5, 4, 5, 12, 16 };

bool isDirectory(const std::string& path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string stripTrailingSlash(std::string path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

template<typename T>
T readBinary(std::ifstream& fin)
{
    T value;
    fin.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

std::string readNullTerminatedString(std::ifstream& fin)
{
    std::string result;
    char c = 0;
    while (fin.read(&c, 1) && c != '\0') result += c;
    return result;
}

struct RawCamera
{
    int id = -1, model = -1;
    uint64_t width = 0, height = 0;
    std::vector<double> params;
};

struct RawImage
{
    int id = -1, cameraId = -1;
    double qvec[4] = { 1.0, 0.0, 0.0, 0.0 };
    double tvec[3] = { 0.0, 0.0, 0.0 };
    std::string name;
};

struct RawPoint
{
    double xyz[3] = { 0.0, 0.0, 0.0 };
    uint8_t rgb[3] = { 128, 128, 128 };
};

bool readCamerasBinary(const std::string& file, std::vector<RawCamera>& out, std::string& error)
{
    std::ifstream fin(file.c_str(), std::ios::binary);
    if (!fin) { error = "Failed to open " + file; return false; }
    uint64_t count = readBinary<uint64_t>(fin);
    out.clear(); out.reserve((size_t)count);
    for (uint64_t i = 0; i < count; ++i)
    {
        RawCamera c;
        c.id = readBinary<int32_t>(fin);
        c.model = readBinary<int32_t>(fin);
        c.width = readBinary<uint64_t>(fin);
        c.height = readBinary<uint64_t>(fin);
        if (c.model < 0 || c.model >= 12)
        {
            error = "Unsupported COLMAP camera model id " + std::to_string(c.model);
            return false;
        }
        int numParams = kCameraModelNumParams[c.model];
        c.params.resize(numParams);
        fin.read(reinterpret_cast<char*>(c.params.data()), numParams * sizeof(double));
        if (!fin) { error = "Truncated cameras.bin"; return false; }
        out.push_back(c);
    }
    return true;
}

bool readCamerasText(const std::string& file, std::vector<RawCamera>& out, std::string& error)
{
    std::ifstream fin(file.c_str());
    if (!fin) { error = "Failed to open " + file; return false; }
    out.clear();
    std::string line;
    while (std::getline(fin, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        RawCamera c;
        std::string modelName;
        if (!(iss >> c.id >> modelName >> c.width >> c.height))
        {
            error = "Malformed cameras.txt"; return false;
        }
        int modelId = -1;
        for (int i = 0; i < 12; ++i)
            if (modelName == kCameraModelNames[i]) { modelId = i; break; }
        if (modelId < 0)
        {
            error = "Unsupported COLMAP camera model " + modelName; return false;
        }
        c.model = modelId;
        int numParams = kCameraModelNumParams[modelId];
        c.params.resize(numParams);
        for (int i = 0; i < numParams; ++i)
            if (!(iss >> c.params[i])) { error = "Malformed camera parameters"; return false; }
        out.push_back(c);
    }
    return true;
}

bool readImagesBinary(const std::string& file, std::vector<RawImage>& out, std::string& error)
{
    std::ifstream fin(file.c_str(), std::ios::binary);
    if (!fin) { error = "Failed to open " + file; return false; }
    uint64_t count = readBinary<uint64_t>(fin);
    out.clear(); out.reserve((size_t)count);
    for (uint64_t i = 0; i < count; ++i)
    {
        RawImage im;
        im.id = readBinary<int32_t>(fin);
        for (int k = 0; k < 4; ++k) im.qvec[k] = readBinary<double>(fin);
        for (int k = 0; k < 3; ++k) im.tvec[k] = readBinary<double>(fin);
        im.cameraId = readBinary<int32_t>(fin);
        im.name = readNullTerminatedString(fin);
        uint64_t numPoints2D = readBinary<uint64_t>(fin);
        fin.seekg((std::streamoff)(numPoints2D * 24), std::ios::cur);  // 2 doubles + uint64
        if (!fin) { error = "Truncated images.bin"; return false; }
        out.push_back(im);
    }
    return true;
}

bool readImagesText(const std::string& file, std::vector<RawImage>& out, std::string& error)
{
    std::ifstream fin(file.c_str());
    if (!fin) { error = "Failed to open " + file; return false; }
    out.clear();
    std::string line;
    while (std::getline(fin, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        RawImage im;
        if (!(iss >> im.id >> im.qvec[0] >> im.qvec[1] >> im.qvec[2] >> im.qvec[3]
                  >> im.tvec[0] >> im.tvec[1] >> im.tvec[2] >> im.cameraId >> im.name))
        {
            error = "Malformed images.txt"; return false;
        }
        out.push_back(im);
        std::getline(fin, line);  // points2D line (may be empty)
    }
    return true;
}

bool readPointsBinary(const std::string& file, std::vector<RawPoint>& out, std::string& error)
{
    std::ifstream fin(file.c_str(), std::ios::binary);
    if (!fin) { error = "Failed to open " + file; return false; }
    uint64_t count = readBinary<uint64_t>(fin);
    out.clear(); out.reserve((size_t)count);
    for (uint64_t i = 0; i < count; ++i)
    {
        readBinary<uint64_t>(fin);  // point3D id
        RawPoint p;
        for (int k = 0; k < 3; ++k) p.xyz[k] = readBinary<double>(fin);
        fin.read(reinterpret_cast<char*>(p.rgb), 3);
        readBinary<double>(fin);  // error
        uint64_t trackLength = readBinary<uint64_t>(fin);
        fin.seekg((std::streamoff)(trackLength * 8), std::ios::cur);  // int32 + int32
        if (!fin) { error = "Truncated points3D.bin"; return false; }
        out.push_back(p);
    }
    return true;
}

bool readPointsText(const std::string& file, std::vector<RawPoint>& out, std::string& error)
{
    std::ifstream fin(file.c_str());
    if (!fin) { error = "Failed to open " + file; return false; }
    out.clear();
    std::string line;
    while (std::getline(fin, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        uint64_t id = 0; RawPoint p;
        int r = 0, g = 0, b = 0;
        if (!(iss >> id >> p.xyz[0] >> p.xyz[1] >> p.xyz[2] >> r >> g >> b))
        {
            error = "Malformed points3D.txt"; return false;
        }
        p.rgb[0] = (uint8_t)std::max(0, std::min(255, r));
        p.rgb[1] = (uint8_t)std::max(0, std::min(255, g));
        p.rgb[2] = (uint8_t)std::max(0, std::min(255, b));
        out.push_back(p);
    }
    return true;
}

bool firstExisting(const std::string& base, const char* const* names, int count, std::string& result)
{
    for (int i = 0; i < count; ++i)
    {
        std::string candidate = base + "/" + names[i];
        if (osgDB::fileExists(candidate)) { result = candidate; return true; }
    }
    return false;
}

std::string getOption(const osgDB::Options* options, const char* name)
{
    return options ? options->getPluginStringData(name) : std::string();
}

}  // namespace


class ReaderWriterCOLMAP : public osgDB::ReaderWriter
{
public:
    ReaderWriterCOLMAP()
    {
        supportsProtocol("colmap", "Read a COLMAP SfM dataset as osgVerse::TrainerInput.");
        supportsExtension("colmap", "Pseudo file extension used to select the COLMAP plugin.");
        supportsOption("imageDir=<name or path>", "Image directory, relative to the dataset root");
        supportsOption("maskDir=<name or path>", "Optional mask directory");
        supportsOption("evalInterval=<n>", "Every n-th image (sorted by name) is used for validation");
        supportsOption("globalScale=<f>", "Additional scene scale factor");
    }

    virtual const char* className() const
    { return "[osgVerse] COLMAP SfM dataset reader for the 3DGS trainer"; }

    virtual ReadResult readObject(const std::string& fileName, const osgDB::Options* options) const
    {
        std::string path = fileName;
        if (osgDB::getServerProtocol(fileName) == "colmap")
        {
            size_t pos = fileName.find("://");
            path = fileName.substr(pos + 3);
        }
        else if (osgDB::getLowerCaseFileExtension(fileName) == "colmap")
        {
            path = osgDB::getNameLessExtension(fileName);
        }
        else return ReadResult::FILE_NOT_HANDLED;

        std::string error;
        osg::ref_ptr<osgVerse::TrainerInput> input = new osgVerse::TrainerInput;
        if (!loadDataset(path, options, input.get(), error))
        {
            OSG_WARN << "[osgdb_colmap] " << error << std::endl;
            return ReadResult::FILE_NOT_HANDLED;
        }
        return ReadResult(input.get());
    }

protected:
    bool loadDataset(const std::string& pathIn, const osgDB::Options* options,
                     osgVerse::TrainerInput* input, std::string& error) const
    {
        const std::string path = stripTrailingSlash(pathIn);

        /* Resolve sparse directory and dataset root */

        std::string sparseDir;
        const char* sparseFiles[] = { "cameras.bin", "cameras.txt" };
        if (firstExisting(path, sparseFiles, 2, sparseDir))
        {
            sparseDir = path;
        }
        else if (firstExisting(path + "/sparse/0", sparseFiles, 2, sparseDir))
        {
            sparseDir = path + "/sparse/0";
        }
        else if (firstExisting(path + "/sparse", sparseFiles, 2, sparseDir))
        {
            sparseDir = path + "/sparse";
        }
        else
        {
            error = "cameras.bin/txt not found under " + path;
            return false;
        }

        std::string root = sparseDir;
        if (endsWith(root, "/sparse/0")) root = root.substr(0, root.size() - 9);
        else if (endsWith(root, "/sparse")) root = root.substr(0, root.size() - 7);

        /* Resolve image directory */

        std::string imageOpt = getOption(options, "imageDir");
        std::string imageDir;
        if (!imageOpt.empty())
        {
            imageDir = isDirectory(imageOpt) ? imageOpt : (root + "/" + imageOpt);
        }
        else
        {
            const char* imageNames[] = {
                "images", "images_png", "images_2", "images_2_png",
                "images_4", "images_4_png", "images_8", "images_8_png"
            };
            for (int i = 0; i < 8 && imageDir.empty(); ++i)
            {
                std::string candidate = root + "/" + imageNames[i];
                if (isDirectory(candidate)) imageDir = candidate;
            }
        }
        if (imageDir.empty() || !isDirectory(imageDir))
        {
            error = "Image directory not found under " + root;
            return false;
        }
        imageDir = stripTrailingSlash(imageDir);

        std::string maskDir = getOption(options, "maskDir");
        if (!maskDir.empty() && !isDirectory(maskDir)) maskDir = root + "/" + maskDir;
        maskDir = stripTrailingSlash(maskDir);

        int evalInterval = 8;
        std::string evalStr = getOption(options, "evalInterval");
        if (!evalStr.empty()) evalInterval = std::max(1, atoi(evalStr.c_str()));
        std::string scaleStr = getOption(options, "globalScale");
        float globalScale = 1.0f;
        if (!scaleStr.empty()) globalScale = (float)atof(scaleStr.c_str());
        input->evalInterval = evalInterval;
        input->globalScale = globalScale;

        /* Read COLMAP files */

        std::vector<RawCamera> cameras;
        std::vector<RawImage> images;
        std::vector<RawPoint> points;
        if (osgDB::fileExists(sparseDir + "/cameras.bin"))
        {
            if (!readCamerasBinary(sparseDir + "/cameras.bin", cameras, error)) return false;
        }
        else if (!readCamerasText(sparseDir + "/cameras.txt", cameras, error)) return false;

        if (osgDB::fileExists(sparseDir + "/images.bin"))
        {
            if (!readImagesBinary(sparseDir + "/images.bin", images, error)) return false;
        }
        else if (!readImagesText(sparseDir + "/images.txt", images, error)) return false;

        if (osgDB::fileExists(sparseDir + "/points3D.bin"))
        {
            if (!readPointsBinary(sparseDir + "/points3D.bin", points, error)) return false;
        }
        else if (osgDB::fileExists(sparseDir + "/points3D.txt"))
        {
            if (!readPointsText(sparseDir + "/points3D.txt", points, error)) return false;
        }
        else
        {
            error = "points3D.bin/txt not found under " + sparseDir;
            return false;
        }

        std::map<int, RawCamera> cameraMap;
        for (size_t i = 0; i < cameras.size(); ++i)
            cameraMap[cameras[i].id] = cameras[i];

        /* Downscale intrinsics when using images_2/4/8 (matching VkSplat/gsplat) */

        int factor = 1;
        std::string imageDirName = osgDB::getSimpleFileName(imageDir);
        const std::pair<const char*, int> factorMap[] = {
            { "images_8", 8 }, { "images_8_png", 8 },
            { "images_4", 4 }, { "images_4_png", 4 },
            { "images_2", 2 }, { "images_2_png", 2 }
        };
        for (size_t i = 0; i < 6; ++i)
            if (imageDirName == factorMap[i].first) factor = factorMap[i].second;

        /* Convert images and split into train/validation sets */

        std::sort(images.begin(), images.end(), [](const RawImage& a, const RawImage& b)
            { return a.name < b.name; });

        for (size_t i = 0; i < images.size(); ++i)
        {
            const RawImage& raw = images[i];
            std::map<int, RawCamera>::const_iterator it = cameraMap.find(raw.cameraId);
            if (it == cameraMap.end())
            {
                error = "Image refers to a missing camera id";
                return false;
            }

            osgVerse::TrainerInput::Camera cam;
            cam.model = (osgVerse::TrainerInput::CameraModel)it->second.model;
            cam.width = (int)(it->second.width / factor);
            cam.height = (int)(it->second.height / factor);
            configureIntrinsics(it->second, (float)factor, cam, error);
            if (!error.empty()) return false;

            // COLMAP stores world-to-camera; the trainer expects camera-to-world
            Eigen::Quaternionf q((float)raw.qvec[0], (float)raw.qvec[1],
                                 (float)raw.qvec[2], (float)raw.qvec[3]);
            Eigen::Matrix4f w2c = Eigen::Matrix4f::Identity();
            w2c.block<3, 3>(0, 0) = q.normalized().toRotationMatrix();
            w2c.block<3, 1>(0, 3) = Eigen::Vector3f((float)raw.tvec[0],
                (float)raw.tvec[1], (float)raw.tvec[2]);
            Eigen::Matrix4f c2w = w2c.inverse();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    cam.camToWorld(r, c) = c2w(r, c);

            cam.imagePath = imageDir + "/" + raw.name;
            if (!maskDir.empty())
            {
                std::vector<std::string> candidates = {
                    maskDir + "/" + raw.name + ".png", maskDir + "/" + raw.name + ".PNG"
                };
                std::string stem = raw.name;
                size_t dot = stem.rfind('.');
                if (dot != std::string::npos)
                {
                    stem = stem.substr(0, dot);
                    candidates.push_back(maskDir + "/" + stem + ".png");
                    candidates.push_back(maskDir + "/" + stem + ".PNG");
                }
                for (size_t k = 0; k < candidates.size(); ++k)
                    if (osgDB::fileExists(candidates[k])) { cam.maskPath = candidates[k]; break; }
            }

            if ((int)(i % (size_t)evalInterval) == 0)
                input->valCameras.push_back(cam);
            else
                input->trainCameras.push_back(cam);
        }

        if (input->trainCameras.empty())
        {
            error = "No training image (check evalInterval)";
            return false;
        }

        /* Points */

        input->points.reserve(points.size());
        input->pointColors.reserve(points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            const RawPoint& p = points[i];
            input->points.push_back(osg::Vec3f((float)p.xyz[0], (float)p.xyz[1], (float)p.xyz[2]));
            input->pointColors.push_back(osg::Vec3f(p.rgb[0] / 255.0f, p.rgb[1] / 255.0f,
                                                    p.rgb[2] / 255.0f));
        }
        return true;
    }

    void configureIntrinsics(const RawCamera& raw, float factor,
                             osgVerse::TrainerInput::Camera& cam, std::string& error) const
    {
        const std::vector<double>& p = raw.params;
        #define _P(i) ((i) < (int)p.size() ? (float)p[i] : 0.0f)
        switch (raw.model)
        {
        case 0:  // SIMPLE_PINHOLE
        case 8:  // SIMPLE_RADIAL_FISHEYE
        case 2:  // SIMPLE_RADIAL
            cam.fx = cam.fy = _P(0); cam.cx = _P(1); cam.cy = _P(2);
            cam.k1 = (raw.model == 2 || raw.model == 8) ? _P(3) : 0.0f;
            break;
        case 1:  // PINHOLE
            cam.fx = _P(0); cam.fy = _P(1); cam.cx = _P(2); cam.cy = _P(3);
            break;
        case 3:  // RADIAL
        case 9:  // RADIAL_FISHEYE
            cam.fx = cam.fy = _P(0); cam.cx = _P(1); cam.cy = _P(2);
            cam.k1 = _P(3); cam.k2 = _P(4);
            break;
        case 4:  // OPENCV
            cam.fx = _P(0); cam.fy = _P(1); cam.cx = _P(2); cam.cy = _P(3);
            cam.k1 = _P(4); cam.k2 = _P(5); cam.p1 = _P(6); cam.p2 = _P(7);
            break;
        case 5:  // OPENCV_FISHEYE
            cam.fx = _P(0); cam.fy = _P(1); cam.cx = _P(2); cam.cy = _P(3);
            cam.k1 = _P(4); cam.k2 = _P(5); cam.k3 = _P(6); cam.k4 = _P(7);
            break;
        case 6:  // FULL_OPENCV
            cam.fx = _P(0); cam.fy = _P(1); cam.cx = _P(2); cam.cy = _P(3);
            cam.k1 = _P(4); cam.k2 = _P(5); cam.p1 = _P(6); cam.p2 = _P(7);
            cam.k3 = _P(8); cam.k4 = _P(9); cam.k5 = _P(10); cam.k6 = _P(11);
            break;
        case 10:  // THIN_PRISM_FISHEYE
            cam.fx = _P(0); cam.fy = _P(1); cam.cx = _P(2); cam.cy = _P(3);
            cam.k1 = _P(4); cam.k2 = _P(5); cam.p1 = _P(6); cam.p2 = _P(7);
            cam.k3 = _P(8); cam.k4 = _P(9); cam.sx1 = _P(10); cam.sy1 = _P(11);
            break;
        case 7:   // FOV
        case 11:  // RAD_TAN_THIN_PRISM_FISHEYE
        default:
            error = "COLMAP camera model " + std::string(kCameraModelNames[raw.model])
                  + " is not supported by the 3DGS trainer";
            return;
        }
        cam.fx /= factor; cam.fy /= factor;
        cam.cx /= factor; cam.cy /= factor;
        #undef _P
    }
};

REGISTER_OSGPLUGIN(colmap, ReaderWriterCOLMAP)
