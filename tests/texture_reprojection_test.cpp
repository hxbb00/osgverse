#include <osg/io_utils>
#include <osg/ArgumentParser>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/Timer>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <iostream>

#include <VerseCommon.h>
#include <pipeline/Utilities.h>
#include <readerwriter/TextureReprojector.h>
#include <readerwriter/Utilities.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

static void printUsage()
{
    std::cout << "Usage: osgVerse_Test_Texture_Reprojection --model <file> [options]\n"
              << "  --out <file.osgb>       save the re-projected geometry, otherwise show it\n"
              << "  --atlas <file.png>      save the baked texture atlas\n"
              << "  --angle <deg>           orbit angular step (default 30)\n"
              << "  --vstart/--vrange <deg> elevation range (default -90 / 180)\n"
              << "  --size <W> | --width/--height <N>   captured image resolution (default 1024)\n"
              << "  --fov <deg>             capture camera field of view (default 60)\n"
              << "  --ratio <0-1>           simplify the mesh before re-projection (rough LOD)\n"
              << "  --max-error <e>         maximum simplification error (default 1.0)\n"
              << "  --no-lock-border        allow simplifying border vertices\n"
              << "  --max-texture <size>    maximum atlas dimension\n"
              << "  --smooth-views <n>      view-selection smoothing iterations\n"
              << "  --padding <n>           atlas patch padding (default 2)\n"
              << "  --inpaint <n>           atlas inpaint radius (default 5)\n"
              << "  --threads <n>           threads used by texture mapping\n"
              << "  --no-occlusion          disable occlusion testing in view selection\n"
              << "  --no-color-correction   disable seam color correction\n"
              << "  --dump-views <prefix>   save every captured orbit view as <prefix>N.png\n"
              << "  --keep-shaders          keep the model's shader programs while capturing\n"
              << "  --view                  show original and re-projected models side by side\n"
              << std::endl;
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    // Standalone example: orbit a complex model with virtual cameras, re-project the rendered
    // views onto its (optionally simplified) geometry with TextureMapping, and produce a new
    // geometry plus one texture atlas.
    // e.g. --model /path/to/model.osgb --out result.osgb --angle 30 --ratio 0.5
    std::string inputFile, outputFile, atlasFile, viewDumpPrefix;
    arguments.read("--model", inputFile);
    if (inputFile.empty() && arguments.argc() > 1 && arguments[1][0] != '-')
        inputFile = arguments[1];
    arguments.read("--out", outputFile);
    arguments.read("--atlas", atlasFile);
    arguments.read("--dump-views", viewDumpPrefix);
    if (inputFile.empty())
    {
        printUsage();
        return 1;
    }

    float angleStep = 30.0f, vStart = -90.0f, vRange = 180.0f, fov = 60.0f;
    int width = 1024, height = 1024, maxTexture = 0;
    float ratio = 0.0f, maxError = 1.0f;
    arguments.read("--angle", angleStep);
    arguments.read("--vstart", vStart);
    arguments.read("--vrange", vRange);
    arguments.read("--fov", fov);
    arguments.read("--size", width); height = width;
    arguments.read("--width", width);
    arguments.read("--height", height);
    arguments.read("--ratio", ratio);
    arguments.read("--max-error", maxError);
    arguments.read("--max-texture", maxTexture);

    // The atlas must be written to a known place when the result is saved as .osgb, as the
    // texture image is referenced by its file name
    if (atlasFile.empty() && !outputFile.empty())
        atlasFile = osgDB::getNameLessExtension(outputFile) + "_tex.png";

    osg::ref_ptr<osg::Node> model = osgDB::readNodeFile(inputFile);
    if (!model.valid())
    {
        std::cout << "Failed to load model: " << inputFile << std::endl;
        return 1;
    }

    osg::Timer timer;
    osgVerse::TextureReprojector reprojector(angleStep, width);
    reprojector.setResolution(width, height);
    reprojector.setVerticalRange(vStart, vRange);
    reprojector.setCameraFov(fov);
    reprojector.setSimplifyRatio(ratio);
    reprojector.setSimplifyMaxError(maxError);
    reprojector.setSimplifyLockBorder(!arguments.read("--no-lock-border"));
    reprojector.setMaxTextureSize(maxTexture);
    reprojector.setStripShaderPrograms(!arguments.read("--keep-shaders"));
    reprojector.setAtlasFile(atlasFile);
    reprojector.setViewDumpPrefix(viewDumpPrefix);

    osgVerse::TextureMapping::MeshTextureMappingOptions& mo = reprojector.getMappingOptions();
    arguments.read("--smooth-views", mo.view_selection_smoothing_iterations);
    arguments.read("--padding", mo.atlas_patch_padding);
    arguments.read("--inpaint", mo.inpaint_radius);
    arguments.read("--threads", mo.num_threads);
    if (arguments.read("--no-occlusion")) mo.enable_occlusion_testing = false;
    if (arguments.read("--no-color-correction")) mo.apply_color_correction = false;

    osg::ref_ptr<osg::Geometry> baked = reprojector.reproject(model.get());
    if (!baked.valid())
    {
        std::cout << "Re-projection failed." << std::endl;
        return 1;
    }

    const osgVerse::TextureMapping::MeshTextureMappingResult& result = reprojector.getResult();
    std::cout << "Re-projected in " << timer.time_m() << "ms with " << reprojector.getNumViews()
              << " orbit views: " << result.face_uvs.size() / 6 << " faces, atlas "
              << result.atlas_width << "x" << result.atlas_height << std::endl;

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(baked.get());
    if (!outputFile.empty())
    {
        if (osgDB::writeNodeFile(*geode, outputFile))
            std::cout << "Saved to " << outputFile << " (texture: " << atlasFile << ")" << std::endl;
        else
            std::cout << "Failed to save " << outputFile << std::endl;
    }

    if (outputFile.empty() || arguments.read("--view"))
    {
        // Show the original model and the re-projected result side by side
        osg::BoundingSphere bs = model->getBound();
        float offset = (bs.radius() > 0.0f ? bs.radius() : 1.0f) * 1.2f;
        osg::ref_ptr<osg::MatrixTransform> left = new osg::MatrixTransform;
        left->setMatrix(osg::Matrix::translate(-offset, 0.0f, 0.0f));
        left->addChild(model.get());

        osg::ref_ptr<osg::MatrixTransform> right = new osg::MatrixTransform;
        right->setMatrix(osg::Matrix::translate(offset, 0.0f, 0.0f));
        right->addChild(geode.get());

        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->addChild(left.get()); root->addChild(right.get());

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
        // GLCore/GLES have no fixed-function pipeline: use an unlit "texture0 * vertex color"
        // program, with a white default texture for untextured models
        osg::StateSet* ss = root->getOrCreateStateSet();
        ss->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        ss->addUniform(new osg::Uniform("baseTexture", (int)0));
        osgVerse::applyTextureAttributeAndModes(
            ss, 0, osgVerse::createDefaultTexture(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f)));
#endif

        osgViewer::Viewer viewer;
        viewer.addEventHandler(new osgViewer::StatsHandler);
        viewer.addEventHandler(new osgViewer::WindowSizeHandler);
        viewer.setCameraManipulator(new osgGA::TrackballManipulator);
        viewer.setSceneData(root.get());
        return viewer.run();
    }
    return 0;
}
