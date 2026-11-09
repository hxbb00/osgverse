#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/VolumetricCloud.h>
using namespace osgVerse;

static bool checkCloudSettings(const VolumetricCloud& cloud) { return true; }

static bool readCloudSettings(osgDB::InputStream& is, VolumetricCloud& cloud)
{
    unsigned int numLayers = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < numLayers; ++i)
    {
        CloudLayerSettings layer; int type = 0;
        is >> is.PROPERTY("Type") >> type;
        is >> is.PROPERTY("Altitude") >> layer.bottomAltitude >> layer.topAltitude;
        is >> is.PROPERTY("Coverage") >> layer.coverage >> layer.density
                                          >> layer.extinction >> layer.albedo;
        is >> is.PROPERTY("Shape") >> layer.shapeAmount >> layer.detailAmount
                                      >> layer.turbulence >> layer.anvilBias;
        is >> is.PROPERTY("HeightGradient") >> layer.heightGradient;
        is >> is.PROPERTY("ShapeBias") >> layer.shapeBias;
        is >> is.PROPERTY("Wind") >> layer.windDirection >> layer.windSpeed;
        is >> is.PROPERTY("Shadow") >> layer.castShadow >> layer.fadeStart >> layer.fadeEnd;
        is >> is.PROPERTY("WeatherMap"); is.readWrappedString(layer.weatherMap);
        is >> is.PROPERTY("ShapeVolume"); is.readWrappedString(layer.shapeVolume);
        is >> is.PROPERTY("DetailVolume"); is.readWrappedString(layer.detailVolume);
        layer.type = (CloudLayerSettings::Type)type;
        cloud.getLayers().push_back(layer);
    }
    is >> is.END_BRACKET;

    CloudRenderSettings settings; int quality = 0;
    is >> is.PROPERTY("Quality") >> quality;
    is >> is.PROPERTY("Render") >> settings.resolutionScale >> settings.maxSteps >> settings.lightSteps
                                     >> settings.shadowCascades >> settings.shadowResolution;
    is >> is.PROPERTY("Switches") >> settings.useCompute >> settings.useTemporal
                                         >> settings.useCloudShadow >> settings.useHalfResolution;
    is >> is.PROPERTY("MaxDistance") >> settings.maxDistance;
    settings.quality = (CloudRenderSettings::Quality)quality;
    cloud.setRenderSettings(settings);

    osg::Vec3 sunDirection, sunColor, ambientColor, planetCenter;
    float planetRadius = 6371000.0f, time = 0.0f;
    bool planetMode = false, frozen = false, jitter = false;
    is >> is.PROPERTY("SunDirection") >> sunDirection;
    is >> is.PROPERTY("SunColor") >> sunColor;
    is >> is.PROPERTY("AmbientColor") >> ambientColor;
    is >> is.PROPERTY("PlanetMode") >> planetMode >> planetCenter >> planetRadius;
    is >> is.PROPERTY("Time") >> time >> frozen;
    is >> is.PROPERTY("TemporalJitter") >> jitter;
    cloud.setSunDirection(sunDirection); cloud.setSunColor(sunColor);
    cloud.setAmbientColor(ambientColor);
    cloud.setPlanetMode(planetMode, planetCenter, planetRadius);
    cloud.setTemporalJitter(jitter);
    cloud.setTime(time); if (!frozen) cloud.setAnimateTime(true);
    return true;
}

static bool writeCloudSettings(osgDB::OutputStream& os, const VolumetricCloud& cloud)
{
    const std::vector<CloudLayerSettings>& layers = cloud.getLayers();
    os.writeSize(layers.size()); os << os.BEGIN_BRACKET << std::endl;
    for (size_t i = 0; i < layers.size(); ++i)
    {
        const CloudLayerSettings& layer = layers[i];
        os << os.PROPERTY("Type") << (int)layer.type << std::endl;
        os << os.PROPERTY("Altitude") << layer.bottomAltitude << layer.topAltitude << std::endl;
        os << os.PROPERTY("Coverage") << layer.coverage << layer.density
                                      << layer.extinction << layer.albedo << std::endl;
        os << os.PROPERTY("Shape") << layer.shapeAmount << layer.detailAmount
                                   << layer.turbulence << layer.anvilBias << std::endl;
        os << os.PROPERTY("HeightGradient") << layer.heightGradient << std::endl;
        os << os.PROPERTY("ShapeBias") << layer.shapeBias << std::endl;
        os << os.PROPERTY("Wind") << layer.windDirection << layer.windSpeed << std::endl;
        os << os.PROPERTY("Shadow") << layer.castShadow << layer.fadeStart << layer.fadeEnd << std::endl;
        os << os.PROPERTY("WeatherMap"); os.writeWrappedString(layer.weatherMap); os << std::endl;
        os << os.PROPERTY("ShapeVolume"); os.writeWrappedString(layer.shapeVolume); os << std::endl;
        os << os.PROPERTY("DetailVolume"); os.writeWrappedString(layer.detailVolume); os << std::endl;
    }
    os << os.END_BRACKET << std::endl;

    const CloudRenderSettings& settings = cloud.getRenderSettings();
    os << os.PROPERTY("Quality") << (int)settings.quality << std::endl;
    os << os.PROPERTY("Render") << settings.resolutionScale << settings.maxSteps << settings.lightSteps
                                << settings.shadowCascades << settings.shadowResolution << std::endl;
    os << os.PROPERTY("Switches") << settings.useCompute << settings.useTemporal
                                  << settings.useCloudShadow << settings.useHalfResolution << std::endl;
    os << os.PROPERTY("MaxDistance") << settings.maxDistance << std::endl;

    osg::Vec3 planetCenter; float planetRadius = 6371000.0f;
    bool planetMode = cloud.getPlanetMode(planetCenter, planetRadius);
    os << os.PROPERTY("SunDirection") << cloud.getSunDirection() << std::endl;
    os << os.PROPERTY("SunColor") << cloud.getSunColor() << std::endl;
    os << os.PROPERTY("AmbientColor") << cloud.getAmbientColor() << std::endl;
    os << os.PROPERTY("PlanetMode") << planetMode << planetCenter << planetRadius << std::endl;
    os << os.PROPERTY("Time") << cloud.getTime() << cloud.getTimeFrozen() << std::endl;
    os << os.PROPERTY("TemporalJitter") << cloud.getTemporalJitter() << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(VolumetricCloud,
                        new osgVerse::VolumetricCloud,
                        osgVerse::VolumetricCloud,
                        "osg::Object osg::Node osg::Group osgVerse::VolumetricCloud")
{
    ADD_USER_SERIALIZER(CloudSettings);
}
