#include <osg/io_utils>
#include <osg/Texture2D>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/FileUtils>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgGA/StateSetManipulator>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgViewer/config/SingleWindow>
#include <iostream>
#include <sstream>

#include <modeling/Math.h>
#include <readerwriter/EarthManipulator.h>
#include <readerwriter/FileCache.h>
#include <readerwriter/DatabasePager.h>
#include <readerwriter/TileCallback.h>
#include <pipeline/IncrementalCompiler.h>
#include <pipeline/ResourceManager.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <pipeline/VolumetricCloud.h>
#include <VerseCommon.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
USE_OSGPLUGIN(verse_tiff)
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

#ifdef false  // no use and will be removed later
static const char* resizeSrc = {
    "#version 430 core \n"
    "layout (local_size_x = 16, local_size_y = 16) in; \n"
    "layout (binding = 0) uniform sampler2D sourceTexture; \n"
    "layout (binding = 1, rgba8) writeonly uniform image2D targetTexture; \n"
    "void main() { \n"
    "    ivec2 targetCoord = ivec2(gl_GlobalInvocationID.xy); \n"
    "    vec2 sourceCoord = vec2(targetCoord) * 2.0 / vec2(textureSize(sourceTexture, 0)); \n"
    "    vec4 color = texture(sourceTexture, sourceCoord); \n"
    "    imageStore(targetTexture, targetCoord, color.bbba); \n"
    "} \n"
};

class UserIncrementalCompileCallback : public osgVerse::IncrementalCompileCallback
{
public:
    UserIncrementalCompileCallback() : osgVerse::IncrementalCompileCallback()
    {
        osg::ref_ptr<osg::Program> resizeProgram = new osg::Program;
        resizeProgram->addShader(new osg::Shader(osg::Shader::COMPUTE, resizeSrc));

        _compute = new osg::DispatchCompute;
        _compute->setDataVariance(osg::Object::DYNAMIC);
        _compute->getOrCreateStateSet()->setAttributeAndModes(resizeProgram.get());
        _compute->getOrCreateStateSet()->addUniform(new osg::Uniform("sourceTexture", (int)0));
        _compute->getOrCreateStateSet()->addUniform(new osg::Uniform("targetTexture", (int)1));
    }

    virtual bool compile(ICO::CompileTextureOp* op, ICO::CompileInfo& compileInfo)
    {
        osg::State* state = compileInfo.getState();
        if (glCopyImageSubData == NULL) osg::setGLExtensionFuncPtr(
            glCopyImageSubData, "glCopyImageSubData", "glCopyImageSubDataEXT", "glCopyImageSubDataNV", true);

        osg::ref_ptr<osg::Texture2D> tex2D = new osg::Texture2D;
        if (op->_texture->getImage(0))
        {
            osg::Image* img = op->_texture->getImage(0);
            tex2D->setTextureSize(img->s() / 2, img->t() / 2);
        }
        else
            tex2D->setTextureSize(op->_texture->getTextureWidth() / 2, op->_texture->getTextureHeight() / 2);
        tex2D->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        tex2D->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        tex2D->setInternalFormat(GL_RGBA8); tex2D->setSourceFormat(GL_RGBA);
        tex2D->setSourceType(GL_UNSIGNED_BYTE);
        if (tex2D->getTextureWidth() == 0 || tex2D->getTextureHeight() == 0)
            return osgVerse::IncrementalCompileCallback::compile(op, compileInfo);

        osg::ref_ptr<osg::Texture::TextureObject> tobj =
            osg::Texture::generateTextureObject(tex2D.get(), state->getContextID(), GL_TEXTURE_2D);
        tex2D->setTextureObject(state->getContextID(), tobj.get());

        osg::ref_ptr<osg::BindImageTexture> binding = new osg::BindImageTexture(
            1, tex2D.get(), osg::BindImageTexture::WRITE_ONLY, GL_RGBA8);
        state->apply(_compute->getStateSet());
        state->applyTextureMode(0, op->_texture->getTextureTarget(), true);
        state->applyTextureAttribute(0, op->_texture.get());
        state->applyTextureMode(1, op->_texture->getTextureTarget(), true);
        state->applyTextureAttribute(1, tex2D.get());
        state->applyAttribute(binding.get());
        _compute->setComputeGroups(
            (tex2D->getTextureWidth() + 15) / 16, (tex2D->getTextureHeight() + 15) / 16, 1);
        _compute->draw(compileInfo); return true;
    }

protected:
    osg::ref_ptr<osg::DispatchCompute> _compute;
};
#endif

const char* finalVertCode = {
    "VERSE_VS_OUT vec4 texCoord; \n"
    "void main() {\n"
    "    texCoord = osg_MultiTexCoord0; \n"
    "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex; \n"
    "}\n"
};

const char* finalFragCode = {
    "uniform sampler2D EarthTexture, OceanTexture, CloudTexture, EarthDepth;\n"
    "uniform float CloudAmount;\n"
    "uniform vec2 CloudSize;\n"
    "uniform int DebugCloud, UseBilateral;\n"
    "VERSE_FS_IN vec4 texCoord; \n"
    "VERSE_FS_OUT vec4 fragColor;\n"

    "vec4 sampleCloud(vec2 uv) {\n"
    "    if (UseBilateral <= 0) return VERSE_TEX2D(CloudTexture, uv);\n"
    "    vec2 cloudPos = uv * CloudSize - 0.5;\n"
    "    vec2 base = floor(cloudPos);\n"
    "    vec2 fraction = cloudPos - base;\n"
    "    float centerDepth = VERSE_TEX2D(EarthDepth, uv).r;\n"
    "    vec4 accum = vec4(0.0); float weightSum = 0.0;\n"
    "    for (int j = 0; j < 2; ++j) {\n"
    "        for (int i = 0; i < 2; ++i) {\n"
    "            vec2 sampleUv = (base + vec2(float(i), float(j)) + 0.5) / CloudSize;\n"
    "            float depth = VERSE_TEX2D(EarthDepth, sampleUv).r;\n"
    "            float weight = (i == 0 ? 1.0 - fraction.x : fraction.x) *\n"
    "                           (j == 0 ? 1.0 - fraction.y : fraction.y);\n"
    "            weight /= 1.0 + 3000.0 * abs(depth - centerDepth);\n"
    "            accum += VERSE_TEX2D(CloudTexture, sampleUv) * weight;\n"
    "            weightSum += weight;\n"
    "        }\n"
    "    }\n"
    "    return accum / max(weightSum, 1e-5);\n"
    "}\n"

    "void main() {\n"
    "    vec4 sceneColor = VERSE_TEX2D(EarthTexture, texCoord.st);\n"
    "    vec4 oceanColor = VERSE_TEX2D(OceanTexture, texCoord.st);\n"
    "    vec4 cloudColor = sampleCloud(texCoord.st);\n"
    "    vec3 baseColor = mix(sceneColor.rgb, oceanColor.rgb, oceanColor.a);\n"
    "    vec3 finalColor = baseColor * mix(1.0, cloudColor.a, CloudAmount)\n"
    "                    + cloudColor.rgb * CloudAmount;\n"
    "    if (DebugCloud == 1) finalColor = vec3(cloudColor.a);\n"
    "    else if (DebugCloud == 2) finalColor = cloudColor.rgb;\n"
    "    else if (DebugCloud == 3) finalColor = baseColor;\n"
    "    fragColor = vec4(finalColor, 1.0); \n"
    "    VERSE_FS_FINAL(fragColor);\n"
    "}\n"
};

static std::string replace(std::string& src, const std::string& match, const std::string& v, bool& c)
{
    size_t levelPos = src.find(match); if (levelPos == std::string::npos) { c = false; return src; }
    src.replace(levelPos, match.length(), v); c = true; return src;
}

static std::string createTengxunPath(int type, const std::string& prefix, int x, int y, int z)
{
    std::string path = prefix; bool changed = false;
    path = replace(path, "{x16}", std::to_string(x / 16), changed);
    path = replace(path, "{y16}", std::to_string(y / 16), changed);
    y = (int)pow((double)z, 2.0) - y - 1; return path;
}

static std::string createMixedPath(int type, const std::string& prefix, int x, int y, int z)
{
    if (type == osgVerse::TileCallback::ORTHOPHOTO)
    {
        if (z > 4)
        {
            std::string prefix2 = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
            return osgVerse::TileCallback::createPath(prefix2, x, pow(2, z) - y - 1, z);
        }
    }
    return osgVerse::TileCallback::createPath(prefix, x, y, z);
}

class EnvironmentHandler : public osgGA::GUIEventHandler
{
public:
    EnvironmentHandler(osgVerse::EarthAtmosphereOcean* e, bool b)
        : _earthData(e), _withOcean(b) {}

    bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
        if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
        {
            _earthData->update(view->getCamera());
            if (_withOcean) _earthData->updateOcean(view->getCamera());
        }
        return false;
    }

protected:
    osgVerse::EarthAtmosphereOcean* _earthData;
    bool _withOcean;
};

int main(int argc, char** argv)
{
    osgVerse::ConsoleHandler* logger = new osgVerse::ConsoleHandler;
#if false
    logger->setShaderLogCallback([](GLenum type, const std::string& name, const std::string& msg)
        {
            std::vector<osg::Shader*> shaders = osgVerse::ResourceManager::instance()->getShaders(name);
            if (shaders.empty()) return name + " [UNKNOWN SHADER ERROR]: " + msg;
            return name + " [SHADER ERROR]: " + msg + "\n==========\n" + shaders[0]->getShaderSource() + "\n==========\n";
        });
#endif
    osg::setNotifyHandler(logger);

    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgDB::Registry::instance()->addFileExtensionAlias("tif", "verse_tiff");
    osgVerse::updateOsgBinaryWrappers();

    bool useOcean = arguments.read("--ocean"), useSky = arguments.read("--sky");
    bool useClouds = arguments.read("--clouds"), withAxes = arguments.read("--with-axes");
    int cloudQuality = (int)osgVerse::CloudRenderSettings::Medium;
    arguments.read("--cloud-quality", cloudQuality);
    int cloudType = (int)osgVerse::CloudLayerSettings::Cumulus;
    arguments.read("--cloud-type", cloudType);
    int cloudDebug = 0; arguments.read("--cloud-debug", cloudDebug);

    // Without --resolution the screen resolution is queried and the scene is shown in a
    // borderless full-screen window; --resolution switches back to a normal window
    int w = 1920, h = 1080;
    bool customResolution = arguments.read("--resolution", w, h);
    if (!customResolution || w <= 0 || h <= 0)
    {
        unsigned int screenW = 0, screenH = 0;
        osgVerse::obtainScreenResolution(screenW, screenH);
        w = (int)(screenW ? screenW : 1280); h = (int)(screenH ? screenH : 720);
        customResolution = false;
    }

    bool bLeft = arguments.read("--image-bottomleft"), wgs84 = arguments.read("--web-wgs84");
    bool usePolar = arguments.read("--polar"), use2D = arguments.read("--map2d");
    bool usePolarNorth = usePolar || arguments.read("--polar-north");
    bool usePolarSouth = usePolar || arguments.read("--polar-south");
    bool usePolarCaps = (usePolarNorth || usePolarSouth) && !use2D;
    if (use2D && usePolar) OSG_NOTICE << "Polar caps are incompatible with 2D maps and will be ignored\n";
    
    std::string startFile, use2Dor3D = use2D ? "UseEarth3D=0" : "UseEarth3D=1";
    std::string useBottomLeftImage = bLeft ? "OriginBottomLeft=1" : "OriginBottomLeft=0";
    std::string useWGS84Tile = wgs84 ? "UseWebMercator=0" : "UseWebMercator=1";
    std::string ortho, elev; arguments.read("--ortho", ortho); arguments.read("--elevation", elev);
    std::string headers, options; arguments.read("--headers", headers); arguments.read("--options", options);
    if (ortho.empty()) ortho = "https://webst01.is.autonavi.com/appmaptile?style%3d6&x%3d{x}&y%3d{y}&z%3d{z}";
    if (!arguments.read("--custom-root", startFile))
        startFile = arguments.read("--multi-root") ? "0-0-x.verse_tms" : "0-0-0.verse_tms";

#if false
    std::string earthURLs = "Orthophoto=http://p0.map.gtimg.com/sateTiles/{z}/{x16}/{y16}/{x}_{y}.jpg UseWebMercator=1";
    osg::ref_ptr<osgDB::Options> earthOptions = new osgDB::Options(earthURLs + " " + use2Dor3D);
    earthOptions->setPluginData("UrlPathFunction", (void*)createTengxunPath);
    osg::ref_ptr<osg::Node> earth = osgDB::readNodeFile("0-0-0.verse_tms", earthOptions.get());
#else
    // Argument examples:
    //   Local tiles from Rivermap: --ortho G:/DOM_DEM/dom/{z}/{x}/{y}.jpg --image-bottomleft --web-wgs84 --multi-root
    //   Local MBTiles from QGIS: --ortho mbtiles://F:/satellite-2017-jpg-z13.mbtiles/{z}-{x}-{y}.jpg
    //                            --elevation mbtiles://F:/elevation-google-tif-z8.mbtiles/{z}-{x}-{y}.tif
    //   GaoDe Map: --ortho "https://webst01.is.autonavi.com/appmaptile?style%3d6&x%3d{x}&y%3d{y}&z%3d{z}"
    //   Google Map: --ortho "https://mt1.google.com/vt/lyrs%3ds&x%3d{x}&y%3d{y}&z%3d{z}"
    //               --elevation "https://mt1.google.com/vt/lyrs%3dt&x%3d{x}&y%3d{y}&z%3d{z}"
    //   MapTiler: --ortho "https://api.maptiler.com/tiles/satellite-v2/{z}/{x}/{y}.jpg?key%3d<your_code>"
    //             --elevation "https://api.maptiler.com/tiles/terrain-quantized-mesh-v2/{z}/{x}/{y}.terrain?key%3d<your_code>"
    //   TianDiTu: --ortho "http://t0.tianditu.gov.cn/img_w/wmts?SERVICE%3dWMTS&REQUEST%3dGetTile&FORMAT%3dtiles&tk%3d<your_code>
    //                      &VERSION%3d1.0.0&LAYER%3dimg&STYLE%3ddefault&TILEMATRIXSET%3dw&TILEMATRIX%3d{z}&TILEROW%3d{y}&TILECOL%3d{x}"
    //             --headers Host;t0.tianditu.gov.cn;Referer;http://www.tianditu.gov.cn
    //   JiLin No.1: --ortho "https://api.jl1mall.com/getMap/{z}/{x}/{y}?mk%3d<your_code>&tk%3d<your_code2>"
    //               --options "FlipVertical=1" --image-bottomleft
    //   GEOVIS Earth: --ortho "https://tiles1.geovisearth.com/base/v1/img/{z}/{x}/{y}?token%3d<your_code>" --options "FlipVertical=1"
    //   Polar caps: --polar (adds polar-stereographic tiles covering |latitude| > 85, so that the
    //                      earth is complete at both poles instead of degenerating into triangles)
    //               --polar-ortho <url> (optional polar-projected imagery, blank image if not given)
    std::string earthURLs = "Orthophoto=" + ortho + (!elev.empty() ? (" Elevation=" + elev + " ") : " ")
                          + useBottomLeftImage + " " + useWGS84Tile + " " + use2Dor3D;
    if (!headers.empty()) earthURLs += " RequestHeaders=" + headers;
    if (!options.empty()) earthURLs += " " + options;

    // The equirectangular / web-mercator pyramid has to stop at the polar circle instead of
    // snapping its polar edge to the pole, so that the polar caps below can continue it
    std::string polarTag = usePolarCaps ? " PolarBoundary=1" : "";
    osg::ref_ptr<osgDB::Options> earthOptions = new osgDB::Options(earthURLs + polarTag);
    osg::ref_ptr<osg::Node> earth = osgDB::readNodeFile(startFile, earthOptions.get());
#endif

    // Polar stereographic caps complete the globe beyond the polar circle, where web-mercator
    // tiles would otherwise degenerate into triangles running out of the poles. Web-mercator
    // imagery does not apply to them, so a placeholder image is used unless a dedicated
    // polar-projected tile source is given with --polar-ortho
    std::string polarOrtho; arguments.read("--polar-ortho", polarOrtho);
    osg::ref_ptr<osg::Group> polarCaps;
    if (usePolarCaps)
    {
        polarCaps = new osg::Group; polarCaps->setName("PolarCaps");
        std::string capURLs = useBottomLeftImage + " UseWebMercator=1 UseEarth3D=1";
        if (polarOrtho.empty()) capURLs += " BlankOrthophoto=1";
        else capURLs += " Orthophoto=" + polarOrtho;
        if (!headers.empty()) capURLs += " RequestHeaders=" + headers;
        if (!options.empty()) capURLs += " " + options;

        const bool poles[2] = { usePolarNorth, usePolarSouth };
        for (int i = 0; i < 2; ++i)
        {
            if (!poles[i]) continue;
            osg::ref_ptr<osgDB::Options> capOptions = new osgDB::Options(
                capURLs + " PolarStereo=" + std::string(i == 0 ? "1" : "-1"));
            osg::ref_ptr<osg::Node> cap = osgDB::readNodeFile("0-0-0.verse_tms", capOptions.get());
            if (cap.valid()) polarCaps->addChild(cap.get());
            else OSG_NOTICE << "[earth_test] Failed to create " << (i == 0 ? "north" : "south") << " polar cap\n";
        }
    }

    // Only pass real local files to osgDB::readNodeFiles(): it treats every non-option token as
    // a file name, which would make it try to load option values such as --resolution or
    // --agent-eye numbers (and crash inside the web plugin for long ECEF values)
    std::vector<std::string> extraFiles;
    for (int i = 1; i < arguments.argc(); ++i)
    {
        std::string name = arguments[i];
        if (!osg::ArgumentParser::isOption(name.c_str()) && osgDB::fileExists(name))
            extraFiles.push_back(name);
    }
    osg::ref_ptr<osg::Node> tiles =
        osgDB::readNodeFiles(extraFiles, new osgDB::Options("DisabledPBR=1"));
    if (useOcean || useSky || useClouds)
    {
        std::string mainFolder = BASE_DIR + "/models/Earth"; arguments.read("--folder", mainFolder);
        earthURLs = " Orthophoto=mbtiles://" + mainFolder + "/DOM_lv4.mbtiles/{z}-{x}-{y}.jpg"
                    " Elevation=mbtiles://" + mainFolder + "/DEM_lv3.mbtiles/{z}-{x}-{y}.tif"
                    " OceanMask=mbtiles://" + mainFolder + "/Mask_lv3.mbtiles/{z}-{x}-{y}.tif"
                    " UseWebMercator=1 UseEarth3D=1 OriginBottomLeft=1 TileElevationScale=3 TileSkirtRatio=0.05";
        earthOptions = new osgDB::Options(earthURLs + " " + options + polarTag);
        earthOptions->setPluginData("UrlPathFunction", (void*)createMixedPath);
        earth = osgDB::readNodeFile("0-0-0.verse_tms", earthOptions.get());
    }

    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
    root->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("BaseTexture"));
    root->getOrCreateStateSet()->addUniform(new osg::Uniform("BaseTexture", (int)0));
#else
    root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif
    if (!earth) { std::cout << "Failed to create earth: " << earthURLs << "\n"; return 1; }

    // Create sky, ocean and clouds
    osgVerse::EarthAtmosphereOcean earthRenderingUtils;
    if (useOcean || useSky || useClouds)
    {
        if (!use2D)
        {
            // Initialize earth utilities
            earthRenderingUtils.create(BASE_DIR + "/textures/transmittance.raw",
                                       BASE_DIR + "/textures/irradiance.raw",
                                       BASE_DIR + "/textures/sunglare.png",
                                       BASE_DIR + "/textures/inscatter.raw");

            // Add earth globe to RTT camera
            osg::ref_ptr<osg::Texture> earthColorBuffer =
                osgVerse::Pipeline::createTexture(osgVerse::Pipeline::RGBA_INT8, w, h);
            osg::ref_ptr<osg::Texture> earthMaskBuffer =
                osgVerse::Pipeline::createTexture(osgVerse::Pipeline::R_INT8, w, h);
            osg::ref_ptr<osg::Texture2D> earthDepthBuffer;
            if (useClouds) earthDepthBuffer = osgVerse::VolumetricCloud::createDepthTexture(w, h);

            osg::Camera* earthCamera = osgVerse::createRTTCamera(osg::Camera::COLOR_BUFFER0, NULL, NULL, false);
            earthCamera->attach(osg::Camera::COLOR_BUFFER0, earthColorBuffer.get(), 0, 0, false, 16, 4);
            earthCamera->attach(osg::Camera::COLOR_BUFFER1, earthMaskBuffer.get());
            if (earthDepthBuffer.valid()) earthCamera->attach(osg::Camera::DEPTH_BUFFER, earthDepthBuffer.get());
            earthCamera->setViewport(0, 0, w, h);
            if (tiles.valid()) earthCamera->addChild(tiles.get());
            earthCamera->addChild(earth.get());
            if (polarCaps.valid()) earthCamera->addChild(polarCaps.get());

            osg::Texture* defTex0 = osgVerse::createDefaultTexture(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
            osg::Texture* defTex1 = osgVerse::createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
            earthRenderingUtils.applyToGlobe(earthCamera->getOrCreateStateSet(), defTex0, defTex0, defTex1,
                    osgDB::readShaderFile(osg::Shader::VERTEX, SHADER_DIR + "scattering_globe.vert.glsl"),
                    osgDB::readShaderFile(osg::Shader::FRAGMENT, SHADER_DIR + "scattering_globe.frag.glsl"));
            root->addChild(earthCamera);

            // Merge atmosphere with earth color and render them on a screen quad
            osg::ref_ptr<osg::Texture> mergedGlobeBuffer = earthColorBuffer;
            if (useSky)
            {
                mergedGlobeBuffer = osgVerse::Pipeline::createTexture(osgVerse::Pipeline::RGBA_INT8, w, h);
                osg::Camera* skyAndGlobeCamera = osgVerse::createRTTCamera(
                    osg::Camera::COLOR_BUFFER0, mergedGlobeBuffer.get(), NULL, true);
                earthRenderingUtils.applyToAtmosphere(skyAndGlobeCamera->getOrCreateStateSet(), earthColorBuffer.get(),
                        osgDB::readShaderFile(osg::Shader::VERTEX, SHADER_DIR + "scattering_sky.vert.glsl"),
                        osgDB::readShaderFile(osg::Shader::FRAGMENT, SHADER_DIR + "scattering_sky.frag.glsl"));
                root->addChild(skyAndGlobeCamera);
            }

            // Add the global volumetric cloud layer (spherical shell around the earth)
            osg::ref_ptr<osg::Texture> cloudColorBuffer;
            osg::ref_ptr<osgVerse::VolumetricCloud> clouds;
            if (useClouds)
            {
                clouds = new osgVerse::VolumetricCloud;
                clouds->setRenderSettings(osgVerse::CloudRenderSettings(
                    (osgVerse::CloudRenderSettings::Quality)osg::clampBetween(cloudQuality, 0, 3)));
                clouds->setPlanetMode(true, osg::Vec3(0.0f, 0.0f, 0.0f), 6371000.0f);
                clouds->setSunDirection(osg::Vec3(-1.0f, 0.0f, 0.0f));  // matches WorldSunDir default
                clouds->setSunColor(osg::Vec3(2.80f, 2.65f, 2.45f));
                clouds->setAmbientColor(osg::Vec3(1.00f, 1.05f, 1.15f));
                clouds->setAtmosphereIntegration(earthRenderingUtils.transmittance.get(),
                                                 earthRenderingUtils.irradiance.get(),
                                                 earthRenderingUtils.inscatter.get(),
                                                 earthRenderingUtils.glare.get());

                osgVerse::CloudLayerSettings layer((osgVerse::CloudLayerSettings::Type)
                    osg::clampBetween(cloudType, 0, 4));
                layer.windSpeed = 0.0f;  // ECEF wind advection is handled per-layer in a later step
                clouds->addLayer(layer);
                clouds->createCloudPass(earthDepthBuffer.get(), w, h);
                root->addChild(clouds.get());
                cloudColorBuffer = clouds->getCloudBuffer();
            }

            // Add post-processing ocean camera if necessary
            osg::ref_ptr<osg::Texture> oceanColorBuffer = defTex1;
            if (useOcean)
            {
                osg::Geometry* grid = earthRenderingUtils.createOceanGrid(w, h);
                osg::Geode* grideGeode = new osg::Geode; grideGeode->addDrawable(grid);

                oceanColorBuffer = osgVerse::Pipeline::createTexture(osgVerse::Pipeline::RGBA_INT8, w, h);
                osg::Camera* oceanCamera = osgVerse::createRTTCamera(
                    osg::Camera::COLOR_BUFFER0, oceanColorBuffer.get(), NULL, false);
                oceanCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
                oceanCamera->setProjectionMatrix(osg::Matrix::ortho2D(-1.0, 1.0, -1.0, 1.0));
                oceanCamera->setViewMatrix(osg::Matrix::identity());
                oceanCamera->addChild(grideGeode);

                float seaRoughness = 0.0f;
                earthRenderingUtils.applyToOcean(oceanCamera->getOrCreateStateSet(),
                        earthMaskBuffer.get(), earthRenderingUtils.createOceanWaves(seaRoughness),
                        osgDB::readShaderFile(osg::Shader::VERTEX, SHADER_DIR + "global_ocean.vert.glsl"),
                        osgDB::readShaderFile(osg::Shader::FRAGMENT, SHADER_DIR + "global_ocean.frag.glsl"));
                root->addChild(oceanCamera);
            }

            // Merge results to an HUD camera to show
            osg::Camera* finalCamera = osgVerse::createHUDCamera(
                NULL, w, h, osg::Vec3(), 1.0f, 1.0f, osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f), true);
            osg::StateSet* finalSS = finalCamera->getOrCreateStateSet();
            osg::Texture* cloudTexture = cloudColorBuffer.valid() ? cloudColorBuffer.get()
                : osgVerse::createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            osgVerse::applyTextureAttributeAndModes(finalSS, 0, mergedGlobeBuffer.get());
            osgVerse::applyTextureAttributeAndModes(finalSS, 1, oceanColorBuffer.get());
            osgVerse::applyTextureAttributeAndModes(finalSS, 2, cloudTexture);
            osg::Texture* earthDepthTex = earthDepthBuffer.valid() ? earthDepthBuffer.get()
                : osgVerse::createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            osgVerse::applyTextureAttributeAndModes(finalSS, 3, earthDepthTex);
            finalSS->addUniform(new osg::Uniform("EarthTexture", (int)0));
            finalSS->addUniform(new osg::Uniform("OceanTexture", (int)1));
            finalSS->addUniform(new osg::Uniform("CloudTexture", (int)2));
            finalSS->addUniform(new osg::Uniform("EarthDepth", (int)3));
            finalSS->addUniform(new osg::Uniform("CloudAmount", cloudColorBuffer.valid() ? 1.0f : 0.0f));
            finalSS->addUniform(new osg::Uniform("CloudSize", clouds.valid() ?
                clouds->getCloudBufferSize() : osg::Vec2(1.0f, 1.0f)));
            finalSS->addUniform(new osg::Uniform("UseBilateral", cloudColorBuffer.valid() ? 1 : 0));
            finalSS->addUniform(new osg::Uniform("DebugCloud", cloudDebug));
            root->addChild(finalCamera);

            osg::ref_ptr<osg::Program> program = new osg::Program;
            {
                osg::Shader* vs = new osg::Shader(osg::Shader::VERTEX, finalVertCode);
                osg::Shader* fs = new osg::Shader(osg::Shader::FRAGMENT, finalFragCode);
                program->addShader(vs); program->addShader(fs);

                int cxtVer = 0, glslVer = 0; osgVerse::guessOpenGLVersions(cxtVer, glslVer);
                osgVerse::Pipeline::createShaderDefinitions(vs, cxtVer, glslVer);
                osgVerse::Pipeline::createShaderDefinitions(fs, cxtVer, glslVer);
            }
            finalCamera->getOrCreateStateSet()->setAttributeAndModes(program.get());
        }
        else
        { OSG_NOTICE << "Ocean, sky and cloud rendering effects are incompatible with 2D maps\n"; return 1; }
    }
    else
    {
        root->addChild(earth.get());
        if (polarCaps.valid()) root->addChild(polarCaps.get());
        if (tiles.valid()) root->addChild(tiles.get());
    }

    // Show axes all over the world for testing
    if (withAxes)
    {
        osg::ref_ptr<osg::Node> axes = osgDB::readNodeFile("axes.osgt.10000,10000,10000.scale");
        for (int y = -88; y <= 88; y += 2)
            for (int x = -180; x < 180; x += 5)
            {
                osg::MatrixTransform* mt = new osg::MatrixTransform;
                mt->setMatrix(osgVerse::Coordinate::convertLLAtoENU(
                    osg::Vec3d(osg::inDegrees((double)y), osg::inDegrees((double)x), 0.0)));
                mt->addChild(axes.get()); root->addChild(mt);
            }
    }

    // Create camera manipulator
    osg::ref_ptr<osgGA::CameraManipulator> camManipulator;
    if (use2D || arguments.read("--trackball"))
        camManipulator = new osgGA::TrackballManipulator;
    else
    {
        osg::ref_ptr<osgVerse::EarthManipulator> manipulator = new osgVerse::EarthManipulator;
        manipulator->setWorldNode(earth.get()); camManipulator = manipulator;
        if (tiles.valid())
        {
            osg::Vec3 dir = tiles->getBound().center(); dir.normalize();
            osg::ref_ptr<osg::MatrixTransform> tileMT = new osg::MatrixTransform;
            //tileMT->setMatrix(osg::Matrix::translate(-dir * 380.0f));
            tileMT->addChild(tiles.get());

            osg::ref_ptr<osg::EllipsoidModel> em = new osg::EllipsoidModel;
            osg::Vec3d ecef = tiles->getBound().center(), newEcef, lla;
            em->convertXYZToLatLongHeight(ecef[0], ecef[1], ecef[2], lla[0], lla[1], lla[2]);

            osg::Vec3d newLLA = osgVerse::Coordinate::convertWGS84toGCJ02(lla);
            std::cout << "WGS84: " << osg::RadiansToDegrees(lla[1]) << ", " << osg::RadiansToDegrees(lla[0]) << "; "
                      << "GCJ02: " << osg::RadiansToDegrees(newLLA[1]) << ", " << osg::RadiansToDegrees(newLLA[0]) << std::endl;
            em->convertLatLongHeightToXYZ(newLLA[0], newLLA[1], newLLA[2], newEcef[0], newEcef[1], newEcef[2]);

            osg::ref_ptr<osg::MatrixTransform> tileOffset = new osg::MatrixTransform;
            tileOffset->setMatrix(osg::Matrix::translate(newEcef - ecef));
            tileOffset->addChild(tileMT.get());
            root->addChild(tileOffset.get());

            osg::BoundingSphere bs = tiles->getBound(); double r = bs.radius() * 10.0;
            manipulator->osgGA::CameraManipulator::setHomePosition(bs.center() + osg::Z_AXIS * r, bs.center(), osg::Y_AXIS);
        }
    }

    if (arguments.read("--cache"))
    {
        osg::ref_ptr<osgVerse::FileCache> fileCache = new osgVerse::FileCache("test_cache");
        osgDB::Registry::instance()->setFileCache(fileCache.get());
    }

    //osg::ref_ptr<osgVerse::IncrementalCompiler> incrementalCompiler = new osgVerse::IncrementalCompiler;
    //incrementalCompiler->setCompileCallback(new UserIncrementalCompileCallback);

    osgViewer::Viewer viewer;
    viewer.getCamera()->setNearFarRatio(0.00001);
    //viewer.setIncrementalCompileOperation(incrementalCompiler.get());
    if (useOcean || useSky || useClouds) viewer.addEventHandler(new EnvironmentHandler(&earthRenderingUtils, useOcean));
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));
    viewer.setCameraManipulator(camManipulator.get());
    viewer.setSceneData(root.get());
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);

    int screenNo = 0; arguments.read("--screen", screenNo);
    osg::ref_ptr<osgViewer::SingleWindow> windowConfig =
        new osgViewer::SingleWindow(0, 0, w, h, screenNo);
    windowConfig->setWindowDecoration(customResolution);
    viewer.apply(windowConfig.get());
    osgVerse::configureViewerForAgent(arguments, &viewer);
    return viewer.run();
}
