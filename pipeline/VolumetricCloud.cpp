#include "VolumetricCloud.h"
#include "Global.h"
#include "Pipeline.h"
#include "Utilities.h"

#include <osg/Camera>
#include <osg/Math>
#include <osg/Program>
#include <osg/Shader>
#include <osgDB/ReadFile>
#include <osgUtil/CullVisitor>
#include <algorithm>
#include <cmath>
#include <thread>
using namespace osgVerse;

namespace
{
    inline unsigned int hashUInt(unsigned int x)
    {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x;
    }

    inline float hashToFloat(unsigned int h) { return (h & 0x00ffffffu) / 16777215.0f; }
    inline int wrapInt(int v, int p) { int r = v % p; return (r < 0) ? (r + p) : r; }
    inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
    inline float smoothf(float t) { return t * t * (3.0f - 2.0f * t); }

    inline float hash3(int x, int y, int z, int seed)
    {
        unsigned int h = hashUInt((unsigned int)x * 374761393u + (unsigned int)y * 668265263u +
                                  (unsigned int)z * 2147483647u + (unsigned int)seed * 1442695041u);
        return hashToFloat(h);
    }

    inline float gradientDot(int ix, int iy, int iz, int period, int seed,
                             float fx, float fy, float fz)
    {
        unsigned int h = hashUInt((unsigned int)wrapInt(ix, period) * 73856093u ^
                                  (unsigned int)wrapInt(iy, period) * 19349663u ^
                                  (unsigned int)wrapInt(iz, period) * 83492791u ^
                                  (unsigned int)seed * 2654435761u);
        switch (h % 12u)
        {
        case 0:  return fx + fy;   case 1:  return -fx + fy;
        case 2:  return fx - fy;   case 3:  return -fx - fy;
        case 4:  return fx + fz;   case 5:  return -fx + fz;
        case 6:  return fx - fz;   case 7:  return -fx - fz;
        case 8:  return fy + fz;   case 9:  return -fy + fz;
        case 10: return fy - fz;   default: return -fy - fz;
        }
    }

    float perlin3D(float x, float y, float z, int period, int seed)
    {
        int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
        float fx = x - ix, fy = y - iy, fz = z - iz;
        float u = smoothf(fx), v = smoothf(fy), w = smoothf(fz);

        float n000 = gradientDot(ix, iy, iz, period, seed, fx, fy, fz);
        float n100 = gradientDot(ix + 1, iy, iz, period, seed, fx - 1.0f, fy, fz);
        float n010 = gradientDot(ix, iy + 1, iz, period, seed, fx, fy - 1.0f, fz);
        float n110 = gradientDot(ix + 1, iy + 1, iz, period, seed, fx - 1.0f, fy - 1.0f, fz);
        float n001 = gradientDot(ix, iy, iz + 1, period, seed, fx, fy, fz - 1.0f);
        float n101 = gradientDot(ix + 1, iy, iz + 1, period, seed, fx - 1.0f, fy, fz - 1.0f);
        float n011 = gradientDot(ix, iy + 1, iz + 1, period, seed, fx, fy - 1.0f, fz - 1.0f);
        float n111 = gradientDot(ix + 1, iy + 1, iz + 1, period, seed, fx - 1.0f, fy - 1.0f, fz - 1.0f);
        return lerpf(lerpf(lerpf(n000, n100, u), lerpf(n010, n110, u), v),
                     lerpf(lerpf(n001, n101, u), lerpf(n011, n111, u), v), w);
    }

    float worley3D(float x, float y, float z, int period, int seed)
    {
        int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
        float px = x - ix, py = y - iy, pz = z - iz;
        float minDist = 4.0f;
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int cx = wrapInt(ix + dx, period), cy = wrapInt(iy + dy, period),
                        cz = wrapInt(iz + dz, period);
                    float jx = dx + hash3(cx, cy, cz, seed);
                    float jy = dy + hash3(cx, cy, cz, seed + 17);
                    float jz = dz + hash3(cx, cy, cz, seed + 31);
                    float ddx = jx - px, ddy = jy - py, ddz = jz - pz;
                    minDist = std::min(minDist, ddx * ddx + ddy * ddy + ddz * ddz);
                }
            }
        }
        return 1.0f - std::min(1.0f, std::sqrt(minDist));
    }

    float fbmPerlin(float x, float y, float z, int period, int octaves, int seed)
    {
        float sum = 0.0f, amp = 0.5f, freq = 1.0f;
        for (int i = 0; i < octaves; ++i)
        {
            sum += amp * perlin3D(x * freq, y * freq, z * freq, period * (int)freq, seed + i * 13);
            amp *= 0.5f; freq *= 2.0f;
        }
        return sum;
    }

    float fbmWorley(float x, float y, float z, int period, int octaves, int seed)
    {
        float sum = 0.0f, amp = 0.625f, freq = 1.0f, total = 0.0f;
        for (int i = 0; i < octaves; ++i)
        {
            sum += amp * worley3D(x * freq, y * freq, z * freq, period * (int)freq, seed + i * 19);
            total += amp; amp *= 0.4f; freq *= 2.0f;
        }
        return sum / std::max(total, 1e-6f);
    }

    float valueNoise2D(float x, float y, int period, int seed)
    {
        int ix = (int)std::floor(x), iy = (int)std::floor(y);
        float fx = x - ix, fy = y - iy, u = smoothf(fx), v = smoothf(fy);
        float n00 = hash3(wrapInt(ix, period), wrapInt(iy, period), 0, seed);
        float n10 = hash3(wrapInt(ix + 1, period), wrapInt(iy, period), 0, seed);
        float n01 = hash3(wrapInt(ix, period), wrapInt(iy + 1, period), 0, seed);
        float n11 = hash3(wrapInt(ix + 1, period), wrapInt(iy + 1, period), 0, seed);
        return lerpf(lerpf(n00, n10, u), lerpf(n01, n11, u), v);
    }

    float fbm2D(float x, float y, int period, int octaves, int seed)
    {
        float sum = 0.0f, amp = 0.5f, freq = 1.0f;
        for (int i = 0; i < octaves; ++i)
        {
            sum += amp * valueNoise2D(x * freq, y * freq, period * (int)freq, seed + i * 23);
            amp *= 0.5f; freq *= 2.0f;
        }
        return sum;
    }

    class CloudCullCallback : public osg::NodeCallback
    {
    public:
        CloudCullCallback(VolumetricCloud* c) : _cloud(c) {}
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            osgUtil::CullVisitor* cv = dynamic_cast<osgUtil::CullVisitor*>(nv);
            if (cv != NULL && _cloud.valid())
            {
                float t = 0.0f;
                if (nv->getFrameStamp()) t = (float)nv->getFrameStamp()->getSimulationTime();
                _cloud->updateMatrices(osg::Matrix(*cv->getModelViewMatrix()),
                                       osg::Matrix(*cv->getProjectionMatrix()), t);
            }
            traverse(node, nv);
        }

    protected:
        osg::observer_ptr<VolumetricCloud> _cloud;
    };

    template<typename Func>
    void parallelFor(int count, Func func)
    {
#if defined(__EMSCRIPTEN__)
        func(0, count);  // no std::thread support without pthreads in the web build
#else
        unsigned int threadCount = std::min(std::max(1u, std::thread::hardware_concurrency()), 16u);
        if (threadCount <= 1)
        {
            func(0, count); return;
        }
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (unsigned int t = 0; t < threadCount; ++t)
        {
            int begin = count * (int)t / (int)threadCount;
            int end = count * (int)(t + 1) / (int)threadCount;
            threads.push_back(std::thread(func, begin, end));
        }
        for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
#endif
    }

    osg::Texture3D* createVolumeTexture(int size, unsigned char* data)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(size, size, size, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, data,
                        osg::Image::USE_NEW_DELETE);

        osg::ref_ptr<osg::Texture3D> tex = new osg::Texture3D;
        tex->setImage(image.get());
        tex->setFilter(osg::Texture3D::MIN_FILTER, osg::Texture3D::LINEAR);
        tex->setFilter(osg::Texture3D::MAG_FILTER, osg::Texture3D::LINEAR);
        tex->setWrap(osg::Texture3D::WRAP_S, osg::Texture3D::REPEAT);
        tex->setWrap(osg::Texture3D::WRAP_T, osg::Texture3D::REPEAT);
        tex->setWrap(osg::Texture3D::WRAP_R, osg::Texture3D::REPEAT);
        tex->setUseHardwareMipMapGeneration(false);
        return tex.release();
    }
}

CloudLayerSettings::CloudLayerSettings(Type t)
{
    type = t; bottomAltitude = 1200.0f; topAltitude = 3200.0f;
    coverage = 0.55f; density = 0.02f; extinction = 1.0f; albedo = 0.9f;
    shapeAmount = 0.85f; detailAmount = 1.0f; turbulence = 0.15f; anvilBias = 0.0f;
    heightGradient.set(0.0f, 0.1625f, 0.88f, 0.98f);
    shapeBias.set(0.5f, 0.5f, 0.5f, 0.5f);
    windDirection.set(1.0f, 0.0f); windSpeed = 8.0f;
    castShadow = true; fadeStart = 30000.0f; fadeEnd = 80000.0f;
    applyPreset(t);
}

void CloudLayerSettings::applyPreset(Type t)
{
    type = t;
    shapeAmount = 0.85f; detailAmount = 1.0f; turbulence = 0.15f; anvilBias = 0.0f;
    shapeBias.set(0.5f, 0.5f, 0.5f, 0.5f);
    switch (t)
    {
    case Stratus:
        bottomAltitude = 600.0f; topAltitude = 1400.0f;
        coverage = 0.75f; density = 0.015f;
        heightGradient.set(0.0f, 0.1f, 0.2f, 0.3f); break;
    case Stratocumulus:
        bottomAltitude = 1000.0f; topAltitude = 2000.0f;
        coverage = 0.6f; density = 0.02f;
        heightGradient.set(0.02f, 0.2f, 0.48f, 0.625f); break;
    case Cumulonimbus:
        bottomAltitude = 800.0f; topAltitude = 8000.0f;
        coverage = 0.62f; density = 0.028f; anvilBias = 0.85f;
        heightGradient.set(0.0f, 0.08f, 0.95f, 1.0f); break;
    case Cirrus:
        bottomAltitude = 6000.0f; topAltitude = 11000.0f;
        coverage = 0.45f; density = 0.008f;
        shapeAmount = 0.55f; detailAmount = 0.5f;
        shapeBias.set(0.72f, 0.5f, 0.5f, 0.5f);
        heightGradient.set(0.05f, 0.2f, 0.8f, 1.0f); break;
    case Cumulus:
        bottomAltitude = 1200.0f; topAltitude = 3200.0f;
        coverage = 0.55f; density = 0.02f;
        heightGradient.set(0.0f, 0.1625f, 0.88f, 0.98f); break;
    default: break;
    }
}

CloudRenderSettings::CloudRenderSettings(Quality q)
{
    quality = q; resolutionScale = 0.5f; maxSteps = 48; lightSteps = 4;
    shadowCascades = 1; shadowResolution = 512;
    useCompute = false; useTemporal = false; useCloudShadow = false; useHalfResolution = true;
    maxDistance = 60000.0f; applyQuality(q);
}

void CloudRenderSettings::applyQuality(Quality q)
{
    quality = q;
    switch (q)
    {
    case Low:
        resolutionScale = 0.4f; maxSteps = 24; lightSteps = 2;
        maxDistance = 30000.0f; shadowResolution = 256; break;
    case Medium:
        resolutionScale = 0.5f; maxSteps = 48; lightSteps = 4;
        maxDistance = 60000.0f; shadowResolution = 512; break;
    case High:
        resolutionScale = 0.75f; maxSteps = 64; lightSteps = 5;
        maxDistance = 80000.0f; shadowResolution = 1024; break;
    default:
        resolutionScale = 1.0f; maxSteps = 96; lightSteps = 6;
        maxDistance = 100000.0f; shadowResolution = 1024; break;
    }
    useHalfResolution = (resolutionScale < 0.99f);
}

VolumetricCloud::VolumetricCloud(Pipeline* p)
:   osg::Group(), _pipeline(p), _settings(CloudRenderSettings::Medium),
    _sunDirection(0.4f, 0.7f, 0.5f), _sunColor(1.4f, 1.35f, 1.25f),
    _ambientColor(0.18f, 0.22f, 0.3f), _earthOrigin(), _planetCenter(),
    _planetRadius(6371000.0f), _planetMode(false), _time(0.0f), _frame(0),
    _initialized(false), _temporalJitter(false), _timeFrozen(false),
    _width(0), _height(0), _cloudWidth(0), _cloudHeight(0)
{
    _sunDirection.normalize();
    setName("VolumetricCloud");
    setDataVariance(osg::Object::DYNAMIC);
    setCullingActive(false);

    // The update callback must be attached to this group, not to the cloud camera: the cull
    // visitor has already pushed the camera's own (screen-space orthographic) matrices when it
    // reaches the camera node, while the group still sees the view camera matrices
    setCullCallback(new CloudCullCallback(this));
}

void VolumetricCloud::setSunDirection(const osg::Vec3& dir)
{
    _sunDirection = dir;
    if (_sunDirection.length2() < 1e-8f) _sunDirection.set(0.4f, 0.7f, 0.5f);
    _sunDirection.normalize();
}

void VolumetricCloud::setPlanetMode(bool enable, const osg::Vec3& center, float radius)
{
    _planetMode = enable; _planetCenter = center;
    _planetRadius = (radius > 1.0f) ? radius : 6371000.0f;
    if (!_uniforms.empty())
    {
        findUniform("CloudPlanetMode")->set(_planetMode ? 1 : 0);
        findUniform("CloudPlanetCenter")->set(_planetCenter);
        findUniform("CloudPlanetRadius")->set(_planetRadius);
    }
}

void VolumetricCloud::setAtmosphereIntegration(osg::Texture* transmittance,
                                               osg::Texture* irradiance,
                                               osg::Texture* inscatter,
                                               osg::Texture* glare,
                                               const osg::Vec3& earthOrigin)
{
    _atmoTransmittance = transmittance;
    _atmoIrradiance = irradiance;
    _atmoInscatter = inscatter;
    _atmoGlare = glare;
    _earthOrigin = earthOrigin;
}

void VolumetricCloud::setWeatherMap(osg::Texture2D* tex) { _weatherMap = tex; }
void VolumetricCloud::setWeatherVolume(osg::Texture3D* tex) { _weatherVolume = tex; }
void VolumetricCloud::setShapeVolume(osg::Texture3D* tex) { _shapeVolume = tex; }
void VolumetricCloud::setDetailVolume(osg::Texture3D* tex) { _detailVolume = tex; }

void VolumetricCloud::addLayer(const CloudLayerSettings& s)
{
    _layers.push_back(s);
    if (_initialized) updateMatrices(osg::Matrix::identity(), osg::Matrix::identity(), _time);
}

bool VolumetricCloud::createNoiseVolumes(bool force)
{
    if (_shapeVolume.valid() && _detailVolume.valid() && !force) return true;
    const int shapeSize = 64, detailSize = 32;

    // Shape volume: R = Perlin-Worley, G/B/A = Worley FBM in increasing frequencies
    unsigned char* shapeData = new unsigned char[shapeSize * shapeSize * shapeSize * 4];
    parallelFor(shapeSize, [&](int z0, int z1)
    {
        for (int z = z0; z < z1; ++z)
        {
            for (int y = 0; y < shapeSize; ++y)
            {
                for (int x = 0; x < shapeSize; ++x)
                {
                    float u = (x + 0.5f) / shapeSize, v = (y + 0.5f) / shapeSize,
                          w = (z + 0.5f) / shapeSize;
                    float perlin = fbmPerlin(u * 4.0f, v * 4.0f, w * 4.0f, 4, 4, 101);
                    float worley = fbmWorley(u * 4.0f, v * 4.0f, w * 4.0f, 4, 3, 211);
                    perlin = osg::clampBetween(perlin * 0.5f + 0.5f, 0.0f, 1.0f);
                    float pw = osg::clampBetween((perlin - (worley - 1.0f)) /
                                                 (1.0f - (worley - 1.0f) + 1e-5f), 0.0f, 1.0f);

                    float g = fbmWorley(u * 8.0f, v * 8.0f, w * 8.0f, 8, 2, 307);
                    float b = fbmWorley(u * 8.0f, v * 8.0f, w * 8.0f, 8, 2, 401);
                    float a = fbmWorley(u * 16.0f, v * 16.0f, w * 16.0f, 16, 2, 503);
                    unsigned char* dst = shapeData + ((size_t)z * shapeSize * shapeSize +
                                                      (size_t)y * shapeSize + x) * 4;
                    dst[0] = (unsigned char)(osg::clampBetween(pw, 0.0f, 1.0f) * 255.0f);
                    dst[1] = (unsigned char)(osg::clampBetween(g, 0.0f, 1.0f) * 255.0f);
                    dst[2] = (unsigned char)(osg::clampBetween(b, 0.0f, 1.0f) * 255.0f);
                    dst[3] = (unsigned char)(osg::clampBetween(a, 0.0f, 1.0f) * 255.0f);
                }
            }
        }
    });
    _shapeVolume = createVolumeTexture(shapeSize, shapeData);

    // Detail volume: three Worley FBM channels used to erode the base shape
    unsigned char* detailData = new unsigned char[detailSize * detailSize * detailSize * 4];
    parallelFor(detailSize, [&](int z0, int z1)
    {
        for (int z = z0; z < z1; ++z)
        {
            for (int y = 0; y < detailSize; ++y)
            {
                for (int x = 0; x < detailSize; ++x)
                {
                    float u = (x + 0.5f) / detailSize, v = (y + 0.5f) / detailSize,
                          w = (z + 0.5f) / detailSize;
                    float r = fbmWorley(u * 8.0f, v * 8.0f, w * 8.0f, 8, 2, 601);
                    float g = fbmWorley(u * 16.0f, v * 16.0f, w * 16.0f, 16, 2, 701);
                    float b = fbmWorley(u * 32.0f, v * 32.0f, w * 32.0f, 32, 2, 809);
                    unsigned char* dst = detailData + ((size_t)z * detailSize * detailSize +
                                                       (size_t)y * detailSize + x) * 4;
                    dst[0] = (unsigned char)(osg::clampBetween(r, 0.0f, 1.0f) * 255.0f);
                    dst[1] = (unsigned char)(osg::clampBetween(g, 0.0f, 1.0f) * 255.0f);
                    dst[2] = (unsigned char)(osg::clampBetween(b, 0.0f, 1.0f) * 255.0f);
                    dst[3] = 255;
                }
            }
        }
    });
    _detailVolume = createVolumeTexture(detailSize, detailData);
    return true;
}

void VolumetricCloud::createWeatherTexture()
{
    const int size = 256;
    unsigned char* data = new unsigned char[size * size * 4];
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            float coverage = fbm2D(u * 4.0f, v * 4.0f, 4, 5, 901);
            float typeNoise = fbm2D(u * 4.0f, v * 4.0f, 4, 4, 1009);
            float heightNoise = fbm2D(u * 8.0f, v * 8.0f, 8, 3, 1103);
            float c = osg::clampBetween((coverage - 0.32f) / 0.42f, 0.0f, 1.0f);
            c = c * c * (3.0f - 2.0f * c);
            unsigned char* dst = data + ((size_t)y * size + x) * 4;
            dst[0] = (unsigned char)(c * 255.0f);
            dst[1] = (unsigned char)(osg::clampBetween(typeNoise, 0.0f, 1.0f) * 255.0f);
            dst[2] = (unsigned char)(osg::clampBetween(heightNoise, 0.0f, 1.0f) * 255.0f);
            dst[3] = 255;
        }
    }

    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->setImage(size, size, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, data,
                    osg::Image::USE_NEW_DELETE);
    osg::ref_ptr<osg::Texture2D> tex = new osg::Texture2D;
    tex->setImage(image.get());
    tex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
    tex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
    tex->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::REPEAT);
    tex->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::REPEAT);
    tex->setUseHardwareMipMapGeneration(false);
    _weatherMap = tex;
}

void VolumetricCloud::createWeatherVolume()
{
    // A 3D weather volume avoids the equirectangular seam and the pole singularity of the
    // 2D map, which shows up as a star-like pattern when the globe is seen from above
    const int size = 32;
    unsigned char* data = new unsigned char[size * size * size * 4];
    parallelFor(size, [&](int z0, int z1)
    {
        for (int z = z0; z < z1; ++z)
        {
            for (int y = 0; y < size; ++y)
            {
                for (int x = 0; x < size; ++x)
                {
                    float u = (x + 0.5f) / size, v = (y + 0.5f) / size, w = (z + 0.5f) / size;
                    float coverage = fbmPerlin(u * 3.0f, v * 3.0f, w * 3.0f, 3, 5, 1301);
                    float typeNoise = fbmPerlin(u * 4.0f, v * 4.0f, w * 4.0f, 4, 4, 1409);
                    float warpNoise = fbmPerlin(u * 5.0f, v * 5.0f, w * 5.0f, 5, 4, 1511);
                    float c = osg::clampBetween(
                        (coverage * 0.5f + 0.5f - 0.32f) / 0.42f, 0.0f, 1.0f);
                    c = c * c * (3.0f - 2.0f * c);
                    unsigned char* dst = data + ((size_t)z * size * size +
                                                 (size_t)y * size + x) * 4;
                    dst[0] = (unsigned char)(c * 255.0f);
                    dst[1] = (unsigned char)(osg::clampBetween(typeNoise * 0.5f + 0.5f,
                                                              0.0f, 1.0f) * 255.0f);
                    dst[2] = (unsigned char)(osg::clampBetween(warpNoise * 0.5f + 0.5f,
                                                              0.0f, 1.0f) * 255.0f);
                    dst[3] = 255;
                }
            }
        }
    });
    _weatherVolume = createVolumeTexture(size, data);
}

void VolumetricCloud::createUniforms()
{
    std::map<std::string, osg::ref_ptr<osg::Uniform>>& u = _uniforms;
    u["CameraToWorld"] = new osg::Uniform("CameraToWorld", osg::Matrixf());
    u["ScreenToCamera"] = new osg::Uniform("ScreenToCamera", osg::Matrixf());
    u["WorldCameraPos"] = new osg::Uniform("WorldCameraPos", osg::Vec3());
    u["WorldSunDir"] = new osg::Uniform("WorldSunDir", osg::Vec3(_sunDirection));
    u["SunColor"] = new osg::Uniform("SunColor", osg::Vec3(_sunColor));
    u["CloudAmbient"] = new osg::Uniform("CloudAmbient", osg::Vec3(_ambientColor));
    u["CloudTime"] = new osg::Uniform("CloudTime", 0.0f);
    u["CloudFrame"] = new osg::Uniform("CloudFrame", 0.0f);
    u["CloudResolution"] = new osg::Uniform("CloudResolution", osg::Vec2(1.0f, 1.0f));

    u["CloudLayerBottom"] = new osg::Uniform("CloudLayerBottom", 1200.0f);
    u["CloudLayerTop"] = new osg::Uniform("CloudLayerTop", 3200.0f);
    u["CloudCoverage"] = new osg::Uniform("CloudCoverage", 0.55f);
    u["CloudDensity"] = new osg::Uniform("CloudDensity", 0.03f);
    u["CloudExtinction"] = new osg::Uniform("CloudExtinction", 1.0f);
    u["CloudAlbedo"] = new osg::Uniform("CloudAlbedo", 0.9f);
    u["CloudType"] = new osg::Uniform("CloudType", 1.0f);
    u["CloudShapeAmount"] = new osg::Uniform("CloudShapeAmount", 0.85f);
    u["CloudDetailAmount"] = new osg::Uniform("CloudDetailAmount", 1.0f);
    u["CloudTurbulence"] = new osg::Uniform("CloudTurbulence", 0.15f);
    u["CloudAnvilBias"] = new osg::Uniform("CloudAnvilBias", 0.0f);
    u["CloudHeightGradient"] = new osg::Uniform("CloudHeightGradient",
                                                osg::Vec4(0.0f, 0.1625f, 0.88f, 0.98f));
    u["CloudShapeBias"] = new osg::Uniform("CloudShapeBias",
                                           osg::Vec4(0.5f, 0.5f, 0.5f, 0.5f));
    u["CloudWind"] = new osg::Uniform("CloudWind", osg::Vec3(8.0f, 0.0f, 0.0f));

    u["CloudMaxDistance"] = new osg::Uniform("CloudMaxDistance", 60000.0f);
    u["CloudFadeStart"] = new osg::Uniform("CloudFadeStart", 30000.0f);
    u["CloudFadeEnd"] = new osg::Uniform("CloudFadeEnd", 80000.0f);
    u["CloudFadeColor"] = new osg::Uniform("CloudFadeColor", osg::Vec3(0.6f, 0.7f, 0.85f));
    u["CloudShapeScale"] = new osg::Uniform("CloudShapeScale", 0.00003f);
    u["CloudDetailScale"] = new osg::Uniform("CloudDetailScale", 0.00018f);
    u["CloudDetailDistance"] = new osg::Uniform("CloudDetailDistance", 25000.0f);
    u["CloudWeatherScale"] = new osg::Uniform("CloudWeatherScale", 5.0e-7f);
    u["CloudMaxSteps"] = new osg::Uniform("CloudMaxSteps", 48);
    u["CloudLightSteps"] = new osg::Uniform("CloudLightSteps", 4);
    u["CloudUseDetail"] = new osg::Uniform("CloudUseDetail", 1);
    u["CloudUseShadow"] = new osg::Uniform("CloudUseShadow", 0);
    u["CloudUseDepth"] = new osg::Uniform("CloudUseDepth", 0);

    u["CloudPlanetMode"] = new osg::Uniform("CloudPlanetMode", _planetMode ? 1 : 0);
    u["CloudPlanetCenter"] = new osg::Uniform("CloudPlanetCenter", _planetCenter);
    u["CloudPlanetRadius"] = new osg::Uniform("CloudPlanetRadius", _planetRadius);
    u["CloudSceneDepth"] = new osg::Uniform("CloudSceneDepth", 0);
    u["CloudShapeVolume"] = new osg::Uniform("CloudShapeVolume", 1);
    u["CloudDetailVolume"] = new osg::Uniform("CloudDetailVolume", 2);
    u["CloudWeatherMap"] = new osg::Uniform("CloudWeatherMap", 3);
    u["CloudWeatherVolume"] = new osg::Uniform("CloudWeatherVolume", 8);
}

osg::Uniform* VolumetricCloud::findUniform(const std::string& name) const
{
    std::map<std::string, osg::ref_ptr<osg::Uniform>>::const_iterator it = _uniforms.find(name);
    return (it == _uniforms.end()) ? NULL : it->second.get();
}

osg::Shader* VolumetricCloud::loadShader(osg::Shader::Type type, const std::string& file)
{
    std::string fullFile = file;
    if (fullFile.find('/') == std::string::npos) fullFile = SHADER_DIR + file;
    osg::ref_ptr<osg::Shader> shader = osgDB::readShaderFile(type, fullFile);
    if (!shader.valid())
    {
        OSG_WARN << "[VolumetricCloud] Failed to load shader " << fullFile << std::endl;
        return NULL;
    }

    shader->setName(file);
    if (_pipeline.valid()) _pipeline->createShaderDefinitionsFromPipeline(shader.get());
    else
    {
        int cxtVer = 0, glslVer = 0; guessOpenGLVersions(cxtVer, glslVer);
        Pipeline::createShaderDefinitions(shader.get(), cxtVer, glslVer);
    }
    return shader.release();
}

void VolumetricCloud::initialize()
{
    if (_initialized) return;
    if (_layers.empty()) _layers.push_back(CloudLayerSettings());
    if (!_shapeVolume.valid() || !_detailVolume.valid()) createNoiseVolumes();
    if (!_weatherMap.valid()) createWeatherTexture();
    if (!_weatherVolume.valid()) createWeatherVolume();
    createUniforms();

    osg::ref_ptr<osg::Shader> vs = loadShader(osg::Shader::VERTEX, "cloud_raymarch.vert.glsl");
    osg::ref_ptr<osg::Shader> fs = loadShader(osg::Shader::FRAGMENT, "cloud_raymarch.frag.glsl");
    if (vs.valid() && fs.valid())
    {
        _raymarchProgram = new osg::Program;
        _raymarchProgram->addShader(vs.get());
        _raymarchProgram->addShader(fs.get());
        _raymarchProgram->setName("VolumetricCloud_Raymarch");
    }
    _initialized = true;
}

osg::Texture2D* VolumetricCloud::createDepthTexture(int w, int h)
{
    osg::ref_ptr<osg::Texture2D> tex = new osg::Texture2D;
    tex->setTextureSize(w, h);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
    tex->setInternalFormat(GL_DEPTH_COMPONENT16);
#else
    tex->setInternalFormat(GL_DEPTH_COMPONENT24);
#endif
    tex->setSourceFormat(GL_DEPTH_COMPONENT);
    tex->setSourceType(GL_UNSIGNED_INT);
    tex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::NEAREST);
    tex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::NEAREST);
    tex->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
    tex->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);
    return tex.release();
}

osg::Camera* VolumetricCloud::createCloudPass(osg::Texture* sceneDepth, int width, int height)
{
    initialize();
    if (!_raymarchProgram.valid()) return NULL;

    _width = width; _height = height;
    int cw = (int)(width * _settings.resolutionScale + 0.5f);
    int ch = (int)(height * _settings.resolutionScale + 0.5f);
    cw = std::max(cw, 1); ch = std::max(ch, 1);
    _cloudWidth = cw; _cloudHeight = ch;
    _cloudBuffer = Pipeline::createTexture(Pipeline::RGBA_FLOAT16, cw, ch);

    osg::Camera* camera = createRTTCamera(osg::Camera::COLOR_BUFFER0, _cloudBuffer.get(), NULL, true);
    camera->setName("VolumetricCloudPass");
    camera->setRenderOrder(osg::Camera::PRE_RENDER, 1);

    // Choose the atmosphere-aware variant when the scattering LUTs were provided
    osg::Program* program = _raymarchProgram.get();
    bool useAtmosphere = _atmoTransmittance.valid() && _atmoIrradiance.valid() &&
                         _atmoInscatter.valid();
    if (useAtmosphere)
    {
        if (!_raymarchAtmoProgram.valid())
        {
            osg::ref_ptr<osg::Shader> vs = loadShader(osg::Shader::VERTEX, "cloud_raymarch.vert.glsl");
            osg::ref_ptr<osg::Shader> fs = loadShader(osg::Shader::FRAGMENT,
                                                      "cloud_raymarch_atmosphere.frag.glsl");
            if (vs.valid() && fs.valid())
            {
                _raymarchAtmoProgram = new osg::Program;
                _raymarchAtmoProgram->addShader(vs.get());
                _raymarchAtmoProgram->addShader(fs.get());
                _raymarchAtmoProgram->setName("VolumetricCloud_Raymarch_Atmosphere");
            }
        }
        if (_raymarchAtmoProgram.valid()) program = _raymarchAtmoProgram.get();
    }
    camera->getOrCreateStateSet()->setAttribute(program);

    osg::StateSet* ss = camera->getOrCreateStateSet();
    osg::Texture* depthTex = sceneDepth;
    if (depthTex == NULL) depthTex = createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    applyTextureAttributeAndModes(ss, 0, depthTex);
    applyTextureAttributeAndModes(ss, 1, _shapeVolume.get());
    applyTextureAttributeAndModes(ss, 2, _detailVolume.get());
    applyTextureAttributeAndModes(ss, 3, _weatherMap.get());
    applyTextureAttributeAndModes(ss, 8, _weatherVolume.get());
    if (useAtmosphere)
    {
        applyTextureAttributeAndModes(ss, 4, _atmoTransmittance.get());
        applyTextureAttributeAndModes(ss, 5, _atmoIrradiance.get());
        applyTextureAttributeAndModes(ss, 6, _atmoInscatter.get());
        applyTextureAttributeAndModes(ss, 7, _atmoGlare.valid() ? _atmoGlare.get()
                                                                : _atmoIrradiance.get());
        ss->getOrCreateUniform("TransmittanceSampler", osg::Uniform::INT)->set(4);
        ss->getOrCreateUniform("SkyIrradianceSampler", osg::Uniform::INT)->set(5);
        ss->getOrCreateUniform("InscatterSampler", osg::Uniform::INT)->set(6);
        ss->getOrCreateUniform("GlareSampler", osg::Uniform::INT)->set(7);
        ss->getOrCreateUniform("EarthOrigin", osg::Uniform::FLOAT_VEC3)->set(_earthOrigin);
    }

    findUniform("CloudUseDepth")->set(sceneDepth != NULL ? 1 : 0);
    findUniform("CloudResolution")->set(osg::Vec2((float)cw, (float)ch));
    findUniform("CloudMaxSteps")->set(_settings.maxSteps);
    findUniform("CloudLightSteps")->set(_settings.lightSteps);
    findUniform("CloudMaxDistance")->set(_settings.maxDistance);
    for (std::map<std::string, osg::ref_ptr<osg::Uniform>>::iterator it = _uniforms.begin();
         it != _uniforms.end(); ++it) ss->addUniform(it->second.get());

    _cloudCamera = camera;
    addChild(camera);
    return camera;
}

osg::Program* VolumetricCloud::applyComposite(osg::StateSet* ss, osg::Texture* sceneColor,
                                               osg::Texture* cloudBuffer, osg::Texture* sceneDepth,
                                               int startUnit, Pipeline* refPipeline)
{
    if (!ss || !sceneColor || !cloudBuffer) return NULL;
    (void)refPipeline;
    if (!_compositeProgram.valid())
    {
        osg::ref_ptr<osg::Shader> vs = loadShader(osg::Shader::VERTEX, "cloud_composite.vert.glsl");
        osg::ref_ptr<osg::Shader> fs = loadShader(osg::Shader::FRAGMENT, "cloud_composite.frag.glsl");
        if (!vs.valid() || !fs.valid()) return NULL;
        _compositeProgram = new osg::Program;
        _compositeProgram->addShader(vs.get());
        _compositeProgram->addShader(fs.get());
        _compositeProgram->setName("VolumetricCloud_Composite");
    }

    ss->setAttribute(_compositeProgram.get());
    osg::Texture* depthTex = sceneDepth;
    if (depthTex == NULL) depthTex = createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    applyTextureAttributeAndModes(ss, startUnit + 0, sceneColor);
    applyTextureAttributeAndModes(ss, startUnit + 1, cloudBuffer);
    applyTextureAttributeAndModes(ss, startUnit + 2, depthTex);
    ss->getOrCreateUniform("SceneColor", osg::Uniform::INT)->set(startUnit + 0);
    ss->getOrCreateUniform("CloudBuffer", osg::Uniform::INT)->set(startUnit + 1);
    ss->getOrCreateUniform("SceneDepth", osg::Uniform::INT)->set(startUnit + 2);
    ss->getOrCreateUniform("UseBilateral", osg::Uniform::INT)->set(sceneDepth != NULL ? 1 : 0);
    ss->getOrCreateUniform("CloudBufferSize", osg::Uniform::FLOAT_VEC2)->set(getCloudBufferSize());
    ss->getOrCreateUniform("DebugMode", osg::Uniform::INT)->set(0);
    return _compositeProgram.get();
}

void VolumetricCloud::update(osg::Camera* camera)
{
    if (!camera) return;
    osg::Matrix view = camera->getViewMatrix();
    osg::Matrix proj = camera->getProjectionMatrix();
    updateMatrices(view, proj, _time);
}

void VolumetricCloud::updateMatrices(const osg::Matrix& viewMatrix,
                                     const osg::Matrix& projMatrix, float simulationTime)
{
    if (_uniforms.empty()) return;
    if (!_timeFrozen) _time = simulationTime;

    osg::Matrix invView = osg::Matrix::inverse(viewMatrix);
    osg::Vec3 camPos(invView.getTrans());
    findUniform("CameraToWorld")->set(osg::Matrixf(invView));
    findUniform("ScreenToCamera")->set(osg::Matrixf(osg::Matrix::inverse(projMatrix)));
    findUniform("WorldCameraPos")->set(camPos);
    findUniform("WorldSunDir")->set(_sunDirection);
    findUniform("SunColor")->set(_sunColor);
    findUniform("CloudAmbient")->set(_ambientColor);
    findUniform("CloudTime")->set(_time);
    findUniform("CloudFrame")->set(_temporalJitter ? (float)(_frame++ % 1024) : 0.0f);
    findUniform("CloudPlanetMode")->set(_planetMode ? 1 : 0);
    findUniform("CloudPlanetCenter")->set(_planetCenter);
    findUniform("CloudPlanetRadius")->set(_planetRadius);
    findUniform("CloudMaxSteps")->set(_settings.maxSteps);
    findUniform("CloudLightSteps")->set(_settings.lightSteps);
    findUniform("CloudMaxDistance")->set(_settings.maxDistance);

    // The noise tile period is 1/scale in world units: the globe needs a domain that is
    // orders of magnitude larger than a local scene, otherwise the tile repeats visibly
    // when the cloud deck is seen from above. Detail noise aliases at a long distance and
    // is only evaluated close to the camera, with a threshold derived from the altitude
    float shapeScale = _planetMode ? 0.000006f : 0.00003f;
    float detailScale = _planetMode ? 0.00006f : 0.00018f;
    float detailDistance = 25000.0f;
    if (_planetMode)
    {
        float altitude = (camPos - _planetCenter).length() - _planetRadius;
        detailDistance = osg::clampBetween(altitude * 0.02f, 20000.0f, 200000.0f);
    }
    findUniform("CloudShapeScale")->set(shapeScale);
    findUniform("CloudDetailScale")->set(detailScale);
    findUniform("CloudDetailDistance")->set(detailDistance);
    findUniform("CloudWeatherScale")->set(2.0e-6f);  // globe mode only: ~500km period

    if (!_layers.empty())
    {
        const CloudLayerSettings& layer = _layers[0];
        findUniform("CloudLayerBottom")->set(layer.bottomAltitude);
        findUniform("CloudLayerTop")->set(layer.topAltitude);
        findUniform("CloudCoverage")->set(layer.coverage);
        findUniform("CloudDensity")->set(layer.density);
        findUniform("CloudExtinction")->set(layer.extinction);
        findUniform("CloudAlbedo")->set(layer.albedo);
        findUniform("CloudShapeAmount")->set(layer.shapeAmount);
        findUniform("CloudDetailAmount")->set(layer.detailAmount);
        findUniform("CloudTurbulence")->set(layer.turbulence);
        findUniform("CloudAnvilBias")->set(layer.anvilBias);
        findUniform("CloudHeightGradient")->set(layer.heightGradient);
        findUniform("CloudShapeBias")->set(layer.shapeBias);
        findUniform("CloudWind")->set(osg::Vec3(layer.windDirection.x() * layer.windSpeed,
                                               layer.windDirection.y() * layer.windSpeed, 0.0f));
        // The distance fade hides the hard border of a local slab; the globe is already
        // bounded by its shell, and applying the local fade would wash the clouds out
        findUniform("CloudFadeStart")->set(_planetMode ? 1.0e9f : layer.fadeStart);
        findUniform("CloudFadeEnd")->set(_planetMode ? 2.0e9f : layer.fadeEnd);
        findUniform("CloudType")->set((float)layer.type);
    }
}
