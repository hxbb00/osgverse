// Tests the vendored VkSplat 3DGS trainer end-to-end:
//   COLMAP dataset -> Vulkan training -> PLY / GaussianGeometry -> OpenGL rendering.
//
// Test data (official COLMAP sample "south-building"):
//   Download: https://demuc.de/colmap/datasets/  (south-building.zip)
//   Structure after extraction:
//     south-building/
//       images/                     # 128 JPG images, 3072x2304
//       sparse/cameras.txt          # SIMPLE_RADIAL, 3072x2304
//       sparse/images.txt
//       sparse/points3D.txt         # ~61.5k points
//   A local copy used for validation:
//     /home/wangrui/Public/Assets/3dgs-data/colmap-south-building
//
//   The loader also accepts the standard sparse/0/... layout and .bin files.
//   Full-resolution images are loaded eagerly; for quick tests create a
//   downscaled images_4/ (768x576) next to images/:
//     python3 - <<'EOF'
//     from PIL import Image; import glob, os
//     os.makedirs('images_4', exist_ok=True)
//     for f in sorted(glob.glob('images/*.JPG')):
//         Image.open(f).convert('RGB').resize((768, 576), Image.LANCZOS).save(
//             'images_4/' + os.path.basename(f), quality=95)
//     EOF
//   The "..._4" suffix makes the loader divide the COLMAP intrinsics by 4.
//
// Test method (run from an installed SDK so the colmap plugin and shaders are found):
//   cd build/sdk/bin
//   # 1) smoke test: load, train and build GaussianGeometry (no rendering)
//   ./osgVerse_Test_Gaussian_Training \
//       --dataset colmap:///home/wangrui/Public/Assets/3dgs-data/colmap-south-building \
//       --image-dir images_4 --steps 20 --no-render
//   # 2) longer run (densification starts at step 500) + PLY output
//   ./osgVerse_Test_Gaussian_Training \
//       --dataset colmap:///home/wangrui/Public/Assets/3dgs-data/colmap-south-building \
//       --image-dir images_4 --steps 700 --output /tmp/south_building.ply --no-render
//   # 3) train and capture a rendered image
//   ./osgVerse_Test_Gaussian_Training \
//       --dataset colmap:///home/wangrui/Public/Assets/3dgs-data/colmap-south-building \
//       --image-dir images_4 --steps 700 \
//       --agent --agent-frames 20 --agent-output /tmp/south_building_shot
//
// Use --no-render to skip the viewer, --mcmc for the MCMC strategy, and the usual
// --agent-* options to capture images. The COLMAP reader also accepts "path.colmap".
#include <osg/io_utils>
#include <osg/ArgumentParser>
#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Group>
#include <osg/Math>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <pipeline/Global.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <modeling/GaussianGeometry.h>
#include <modeling/GaussianTrainer.h>
#include <iostream>

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    std::string dataset, output, imageDir, maskDir, shaderDir;
    int steps = 200, device = -1, evalInterval = 8;
    float globalScale = 1.0f;
    bool useMCMC = arguments.read("--mcmc");
    bool noRender = arguments.read("--no-render");
    arguments.read("--dataset", dataset);
    arguments.read("--steps", steps);
    arguments.read("--device", device);
    arguments.read("--output", output);
    arguments.read("--shader-dir", shaderDir);
    arguments.read("--image-dir", imageDir);
    arguments.read("--mask-dir", maskDir);
    arguments.read("--eval-interval", evalInterval);
    arguments.read("--global-scale", globalScale);

    if (dataset.empty())
    {
        std::cout << "Usage: " << argv[0]
                  << " --dataset <colmap://path|path.colmap> [--steps N] [--device N]\n"
                  << "       [--mcmc] [--output splat.ply] [--shader-dir DIR]\n"
                  << "       [--image-dir NAME] [--mask-dir NAME] [--eval-interval N]\n"
                  << "       [--global-scale F] [--no-render] [--agent ...]\n";
        return 1;
    }
    if (shaderDir.empty()) shaderDir = SHADER_DIR + "3dgs_trainer/";

    osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());

    /* Load the COLMAP dataset through the plugin */

    std::string optString;
    if (!imageDir.empty()) optString += "imageDir=" + imageDir + " ";
    if (!maskDir.empty()) optString += "maskDir=" + maskDir + " ";
    optString += "evalInterval=" + std::to_string(evalInterval);

    osg::ref_ptr<osgDB::Options> options = new osgDB::Options(optString);
    osg::ref_ptr<osg::Object> object = osgDB::readObjectFile(dataset, options.get());
    osgVerse::TrainerInput* input = dynamic_cast<osgVerse::TrainerInput*>(object.get());
    if (input == NULL)
    {
        std::cout << "Failed to read COLMAP dataset: " << dataset
                  << "\n(check the path and make sure osgdb_colmap is installed)" << std::endl;
        return 1;
    }
    std::cout << "Dataset: " << input->getNumTrainImages() << " train, "
              << input->getNumValImages() << " val, " << input->getNumPoints()
              << " initial points" << std::endl;

    /* Train */

    osgVerse::GaussianTrainerParameters params;
    params.shaderDir = shaderDir;
    params.deviceId = device;
    params.maxSteps = steps;
    params.useMCMC = useMCMC;
    params.globalScale = globalScale;

    osg::ref_ptr<osgVerse::GaussianTrainer> trainer = new osgVerse::GaussianTrainer;
    if (!trainer->initialize(input, params))
    {
        std::cout << "Trainer initialization failed: " << trainer->getLastError() << std::endl;
        return 1;
    }

    int lastPrinted = -1;
    while (trainer->step())
    {
        int step = trainer->getCurrentStep();
        if (step / 100 != lastPrinted)
        {
            lastPrinted = step / 100;
            std::cout << "\rStep " << step << "/" << steps
                      << ", splats " << trainer->getNumSplats() << std::flush;
        }
    }
    std::cout << "\nTraining finished: step " << trainer->getCurrentStep()
              << ", splats " << trainer->getNumSplats()
              << ", VRAM " << (trainer->getVRAMUsage() / (1024.0 * 1024.0)) << " MiB"
              << ", peak VRAM " << (trainer->getPeakVRAMUsage() / (1024.0 * 1024.0)) << " MiB"
              << std::endl;

    const std::map<std::string, std::tuple<size_t, double>> timings =
        trainer->getTimingBreakdown();
    for (std::map<std::string, std::tuple<size_t, double>>::const_iterator it = timings.begin();
         it != timings.end(); ++it)
    {
        if (std::get<0>(it->second) == 0) continue;
        std::cout << "  " << it->first << ": " << std::get<1>(it->second) << " s ("
                  << std::get<0>(it->second) << " calls)" << std::endl;
    }

    if (!output.empty())
    {
        if (trainer->savePLY(output))
            std::cout << "PLY written to " << output << std::endl;
        else
            std::cout << "Failed to write PLY: " << trainer->getLastError() << std::endl;
    }

    osg::ref_ptr<osgVerse::GaussianGeometry> geometry = trainer->createGeometry();
    if (geometry == NULL)
    {
        std::cout << "Failed to create GaussianGeometry: " << trainer->getLastError() << std::endl;
        return 1;
    }
    std::cout << "GaussianGeometry: " << geometry->getNumSplats() << " splats" << std::endl;
    if (noRender) return 0;

    /* Render with the existing OpenGL 3DGS path */

    osg::Shader* vs = osgDB::readShaderFile(
        osg::Shader::VERTEX, SHADER_DIR + "gaussian_splatting.vert.glsl");
    osg::Shader* fs = osgDB::readShaderFile(
        osg::Shader::FRAGMENT, SHADER_DIR + "gaussian_splatting.frag.glsl");
    if (!vs || !fs)
    {
        std::cout << "Missing gaussian_splatting shaders under " << SHADER_DIR << std::endl;
        return 1;
    }

    int cxtVer = 0, glslVer = 0;
    osgVerse::guessOpenGLVersions(cxtVer, glslVer);
    glslVer = osg::maximum(glslVer, 430);  // SSBO-based instancing
    vs->setUserValue("Definitions", std::string("USE_INSTANCING"));
    osgVerse::Pipeline::createShaderDefinitions(vs, cxtVer, glslVer);
    osgVerse::Pipeline::createShaderDefinitions(fs, cxtVer, glslVer);

    osg::ref_ptr<osg::Program> program = osgVerse::GaussianGeometry::createProgram(
        vs, NULL, fs, osgVerse::GaussianGeometry::INSTANCING);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(program.get());
    ss->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    ss->setAttributeAndModes(new osg::BlendEquation(osg::BlendEquation::FUNC_ADD));
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false));
    ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    geometry->setCullCallback(osgVerse::GaussianGeometry::createUniformCallback());

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(geometry.get());

    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setSceneData(root.get());
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.getCamera()->setClearColor(osg::Vec4(0.05f, 0.05f, 0.08f, 1.0f));
    viewer.home();

    osg::ref_ptr<osgVerse::GaussianSorter> sorter = new osgVerse::GaussianSorter;
    sorter->addGeometry(geometry.get());
    viewer.getCamera()->setPreDrawCallback(new osgVerse::GaussianSortCallback(sorter.get()));

    osgVerse::configureViewerForAgent(arguments, &viewer);
    return viewer.run();
}
