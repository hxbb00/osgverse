#ifndef OSGVERSE_PIPELINE_VOLUMETRIC_CLOUD_H
#define OSGVERSE_PIPELINE_VOLUMETRIC_CLOUD_H

#include <osg/Camera>
#include <osg/Group>
#include <osg/observer_ptr>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Uniform>
#include <map>
#include <string>
#include <vector>

namespace osgVerse
{
    class Pipeline;

    /** Settings of one cloud layer. Multiple layers can be added to describe e.g. a low
        cumulus deck and a high cirrus deck in the same scene */
    struct CloudLayerSettings
    {
        enum Type
        {
            Stratus = 0, Stratocumulus, Cumulus, Cumulonimbus, Cirrus, Custom
        };

        Type type;
        float bottomAltitude, topAltitude;  // altitude range in meters
        float coverage, density, extinction, albedo;
        float shapeAmount, detailAmount, turbulence, anvilBias;
        osg::Vec4 heightGradient;           // x, y: fade-in range; z, w: fade-out range
        osg::Vec4 shapeBias;
        osg::Vec2 windDirection; float windSpeed;
        bool castShadow; float fadeStart, fadeEnd;
        std::string weatherMap, shapeVolume, detailVolume;  // optional user data

        CloudLayerSettings(Type t = Cumulus);
        void applyPreset(Type t);
    };

    /** Quality and degradation switches of the cloud renderer */
    struct CloudRenderSettings
    {
        enum Quality { Low = 0, Medium, High, Ultra };
        Quality quality;
        float resolutionScale;
        int maxSteps, lightSteps, shadowCascades, shadowResolution;
        bool useCompute, useTemporal, useCloudShadow, useHalfResolution;
        float maxDistance;

        CloudRenderSettings(Quality q = Medium);
        void applyQuality(Quality q);
    };

    /** Volumetric cloud renderer. It owns the noise volumes, the ray-marching RTT camera
        and the compositing shader, and can be used either standalone (like the earth test
        does with EarthAtmosphereOcean) or inside a Pipeline viewer.

        The cloud pass output (getCloudBuffer()) is RGB = in-scattered radiance and
        A = transmittance, so compositing is: scene.rgb * cloud.a + cloud.rgb */
    class VolumetricCloud : public osg::Group
    {
    public:
        VolumetricCloud(Pipeline* p = NULL);
        VolumetricCloud(const VolumetricCloud& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
        META_Node(osgVerse, VolumetricCloud)
        virtual ~VolumetricCloud() {}

        /** Add a cloud layer. Layers are kept in order; the current raymarch pass evaluates
            the first one (additional layers are reserved for the multi-layer pass) */
        void addLayer(const CloudLayerSettings& s);
        void clearLayers() { _layers.clear(); }
        unsigned int getNumLayers() const { return (unsigned int)_layers.size(); }
        std::vector<CloudLayerSettings>& getLayers() { return _layers; }
        const std::vector<CloudLayerSettings>& getLayers() const { return _layers; }

        void setSunDirection(const osg::Vec3& dir);
        const osg::Vec3& getSunDirection() const { return _sunDirection; }
        void setSunColor(const osg::Vec3& color) { _sunColor = color; }
        const osg::Vec3& getSunColor() const { return _sunColor; }
        void setAmbientColor(const osg::Vec3& color) { _ambientColor = color; }
        const osg::Vec3& getAmbientColor() const { return _ambientColor; }

        /** Freeze the cloud animation at the given time. Use setAnimateTime(true) to resume */
        void setTime(float t) { _time = t; _timeFrozen = true; }
        void setAnimateTime(bool enabled) { _timeFrozen = !enabled; }
        float getTime() const { return _time; }
        bool getTimeFrozen() const { return _timeFrozen; }

        /** Per-frame stochastic jitter of the ray-march start. It reduces banding when a
            temporal accumulation pass is available, but without one it shows up as visible
            flicker, so it is disabled by default. The jitter is still per-pixel (IGN) */
        void setTemporalJitter(bool enabled) { _temporalJitter = enabled; }
        bool getTemporalJitter() const { return _temporalJitter; }

        /** Enable the globe/planet mode: cloud layers are spherical shells at
            [radius+bottom, radius+top] instead of a local horizontal slab */
        void setPlanetMode(bool enable, const osg::Vec3& center = osg::Vec3(),
                           float radius = 6371000.0f);
        bool getPlanetMode(osg::Vec3& center, float& radius) const
        { center = _planetCenter; radius = _planetRadius; return _planetMode; }

        /** Optional atmosphere integration: the transmittance/inscatter LUTs (Proland /
            Bruneton, as created by EarthAtmosphereOcean) are used to fade the clouds into
            the atmosphere. Call it before createCloudPass() */
        void setAtmosphereIntegration(osg::Texture* transmittance, osg::Texture* irradiance,
                                      osg::Texture* inscatter, osg::Texture* glare = NULL,
                                      const osg::Vec3& earthOrigin = osg::Vec3());

        /** Optional user data. The renderer creates its own noise and weather textures
            when these are not given, so an external file is not required. The 2D weather
            map is used by the local slab mode, while the 3D one is used by the globe mode
            because an equirectangular projection would collapse at the poles */
        void setWeatherMap(osg::Texture2D* tex);
        void setWeatherVolume(osg::Texture3D* tex);
        void setShapeVolume(osg::Texture3D* tex);
        void setDetailVolume(osg::Texture3D* tex);
        osg::Texture3D* getShapeVolume() const { return _shapeVolume.get(); }
        osg::Texture3D* getDetailVolume() const { return _detailVolume.get(); }
        bool createNoiseVolumes(bool force = false);

        void setRenderSettings(const CloudRenderSettings& s) { _settings = s; }
        const CloudRenderSettings& getRenderSettings() const { return _settings; }

        /** Create the full-screen ray-marching pass. sceneDepth may be NULL, in which case a
            flat ground plane is used to limit the marching range */
        osg::Camera* createCloudPass(osg::Texture* sceneDepth, int width, int height);
        osg::Texture* getCloudBuffer() const { return _cloudBuffer.get(); }
        osg::Vec2 getCloudBufferSize() const
        { return osg::Vec2((float)_cloudWidth, (float)_cloudHeight); }

        /** Apply the compositing program: output = scene.rgb * cloud.a + cloud.rgb.
            When sceneDepth is given, the (possibly lower resolution) cloud buffer is
            upsampled with a depth-aware bilateral filter to avoid halos on silhouettes */
        osg::Program* applyComposite(osg::StateSet* ss, osg::Texture* sceneColor,
                                     osg::Texture* cloudBuffer, osg::Texture* sceneDepth = NULL,
                                     int startUnit = 0, Pipeline* refPipeline = NULL);

        /** Update camera/sun/time uniforms. Call it once per frame with the view camera, or
            rely on the cull callback installed by createCloudPass() */
        void update(osg::Camera* camera);
        void updateMatrices(const osg::Matrix& viewMatrix, const osg::Matrix& projMatrix,
                            float simulationTime);

        /** Create a depth texture suitable for attaching to a scene RTT camera */
        static osg::Texture2D* createDepthTexture(int w, int h);

    protected:
        void initialize();
        void createWeatherTexture();
        void createWeatherVolume();
        void createUniforms();
        osg::Shader* loadShader(osg::Shader::Type type, const std::string& file);
        osg::Uniform* findUniform(const std::string& name) const;

        osg::observer_ptr<Pipeline> _pipeline;
        std::vector<CloudLayerSettings> _layers;
        CloudRenderSettings _settings;
        osg::ref_ptr<osg::Texture2D> _weatherMap;
        osg::ref_ptr<osg::Texture3D> _weatherVolume;
        osg::ref_ptr<osg::Texture3D> _shapeVolume;
        osg::ref_ptr<osg::Texture3D> _detailVolume;
        osg::ref_ptr<osg::Texture> _cloudBuffer;
        osg::observer_ptr<osg::Camera> _cloudCamera;
        osg::ref_ptr<osg::Program> _raymarchProgram, _raymarchAtmoProgram, _compositeProgram;
        osg::ref_ptr<osg::Texture> _atmoTransmittance, _atmoIrradiance, _atmoInscatter, _atmoGlare;
        osg::Vec3 _earthOrigin;
        std::map<std::string, osg::ref_ptr<osg::Uniform>> _uniforms;
        osg::Vec3 _sunDirection, _sunColor, _ambientColor;
        osg::Vec3 _planetCenter; float _planetRadius; bool _planetMode;
        float _time; int _frame;
        bool _initialized, _temporalJitter, _timeFrozen;
        int _width, _height, _cloudWidth, _cloudHeight;
    };
}

#endif
