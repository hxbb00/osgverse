#include <osg/io_utils>
#include <osg/LightSource>
#include <osg/Texture2D>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/FileUtils>
#include <osgGA/TrackballManipulator>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <algorithm>
#include <iostream>
#include <sstream>

#include <VerseCommon.h>
#include <ai/DepthAnythingEngine.h>
#include <ai/FreeSplatterEngine.h>
#include <ai/SkinTokensEngine.h>
#include <ai/TrellisPipeline.h>
#include <ai/Utilities.h>
#include <modeling/GaussianGeometry.h>
#include <modeling/Utilities.h>
#include <pipeline/Drawer2D.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

std::vector<osg::ref_ptr<osg::Image>> obtainImages(const std::string& location, bool flipped)
{
    std::vector<std::string> files = osgDB::getDirectoryContents(location);
    std::vector<osg::ref_ptr<osg::Image>> images;

    if (files.empty())
    {
        osg::ref_ptr<osg::Image> image = osgDB::readImageFile(location);
        if (image.valid()) { if (flipped) image->flipVertical(); images.push_back(image); }
    }
    else
    {
        for (size_t i = 0; i < files.size(); ++i)
        {
            const std::string& f = files[i];
            if (f.empty() || f[0] == '.') continue;

            osg::ref_ptr<osg::Image> image = osgDB::readImageFile(location + "/" + f);
            if (image.valid()) { if (flipped) image->flipVertical(); images.push_back(image); }
        }
    }
    std::cout << "Loaded " << images.size() << " images as input\n";
    return images;
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
    osgVerse::updateOsgBinaryWrappers();

    osg::ApplicationUsage* usage = arguments.getApplicationUsage();
    usage->addCommandLineOption("--image", "Provide an image as input, or a folder with multuple images as inputs.");
    usage->addCommandLineOption("--da3", "Provide a DA3 model, and generate a depth image for given input.");
    usage->addCommandLineOption("--da3-gs", "Provide a DA3 model, and generate an 3DGS splat file for given input.");
    usage->addCommandLineOption("--free-splatter", "Provide a FreeSplatter model, and generate an 3DGS splat file for given inputs.");
    usage->addCommandLineOption("--skin-tokens", "Provide a SkinTokens model directory, and automatically rig+skin the model given by '--character'.");
    usage->addCommandLineOption("--character", "Provide a static character model (.glb/.fbx/...) to be auto-rigged by SkinTokens.");
    usage->addCommandLineOption("--character-save", "Export the generated rig as a self-contained GLB.");
    usage->addCommandLineOption("--device", "Inference device for AI models: auto (default), cpu or vulkan.");
    usage->addCommandLineOption("--bend-joint", "Bend the subtree of the given joint index to check the generated skinning.");
    usage->addCommandLineOption("--bend-angle", "Rotation angle (degrees) used with --bend-joint, default is 45.");

    osg::ref_ptr<osg::Geometry> quad0 = osg::createTexturedQuadGeometry(
        osg::Vec3(0.6f, 0.0f, 0.0f), osg::X_AXIS, osg::Z_AXIS, 0.0f, 0.0f, 1.0f, 1.0f);
    osg::ref_ptr<osg::Geometry> quad1 = osg::createTexturedQuadGeometry(
        osg::Vec3(-0.6f, 0.0f, 0.0f), osg::X_AXIS, osg::Z_AXIS, 0.0f, 0.0f, 1.0f, 1.0f);
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(quad0.get()); geode->addDrawable(quad1.get());
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
    geode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif

    std::vector<std::pair<std::string, bool>> backends = osgVerse::getAvailableBackendsGGML();
    std::cout << "==== Available GGML backend devices ====\n";
    for (size_t i = 0; i < backends.size(); ++i)
        std::cout << "  " << backends[i].first << " " << (backends[i].second ? "(gpu)" : "(cpu)") << "\n";

    std::string dataLocation, modelFile; arguments.read("--image", dataLocation);
    std::string characterFile; arguments.read("--character", characterFile);
    std::string rigSaveFile; arguments.read("--character-save", rigSaveFile);
    std::string deviceName; arguments.read("--device", deviceName);
    int bendJoint = -1; arguments.read("--bend-joint", bendJoint);
    float bendAngle = 45.0f; arguments.read("--bend-angle", bendAngle);

    // The viewer must exist before reading models, so that --agent* options are consumed here
    // and will not be treated as file names
    osgViewer::Viewer viewer;
    bool agentMode = osgVerse::configureViewerForAgent(arguments, &viewer);

    osg::ref_ptr<osg::Node> characterNode;
    osg::ref_ptr<osgVerse::PlayerAnimation> characterPlayer;
    osg::Timer_t startT = osg::Timer::instance()->tick(), loadedT = 0, endT = 0;
    if (arguments.read("--da3", modelFile))
    {   // DA3 based depth estimation
        std::vector<osg::ref_ptr<osg::Image>> images = obtainImages(dataLocation, false);
        if (images.empty()) { std::cerr << "No input image\n"; return 1; }

        osg::ref_ptr<osgVerse::DepthAnything> da3 = new osgVerse::DepthAnything(modelFile);
        loadedT = osg::Timer::instance()->tick();
        osgVerse::DepthAnything::DepthContent content = da3->estimateDepth(*(images[0]), false);
        endT = osg::Timer::instance()->tick();

        if (!content.depth) { std::cerr << "Failed to estimate depth image\n"; return 1; }
        quad0->getOrCreateStateSet()->setTextureAttributeAndModes(0, osgVerse::createTexture2D(images[0].get()));
        quad1->getOrCreateStateSet()->setTextureAttributeAndModes(0, osgVerse::createTexture2D(content.depth.get()));
        root->addChild(geode.get());
    }
    else if (arguments.read("--da3-gs", modelFile))
    {   // DA3-Giant based 3DGS generation
        std::vector<osg::ref_ptr<osg::Image>> images = obtainImages(dataLocation, false);
        if (images.empty()) { std::cerr << "No input image\n"; return 1; }

        osg::Image* image = images[0].get();
        int gsW = 224, gsH = 224; arguments.read("--da3-width", gsW); arguments.read("--da3-height", gsH);
        if (gsW != image->s() || gsH != image->t()) image->scaleImage(gsW, gsH, 1);

        osg::ref_ptr<osgVerse::DepthAnything> da3 = new osgVerse::DepthAnything(modelFile);
        loadedT = osg::Timer::instance()->tick();
        osgVerse::DepthAnything::GaussianContent content = da3->reconstruct(*image);
        endT = osg::Timer::instance()->tick();

        if (!content.positions) { std::cerr << "Failed to reconstruct 3DGS\n"; return 1; }
        osg::ref_ptr<osgVerse::GaussianGeometry> geom = new osgVerse::GaussianGeometry;
        geom->setPosition(content.positions.get());
        geom->setScaleAndRotation(content.scales.get(), content.rotations.get(), content.alphas.get());
        geom->setShRed(0, content.reds.get()); geom->setShGreen(0, content.greens.get());
        geom->setShBlue(0, content.blues.get()); geom->setShDegrees(0);

        osg::ref_ptr<osg::Geode> g = new osg::Geode; g->addDrawable(geom.get());
        if (!osgDB::writeNodeFile(*g, "da3_gs_result.splat.verse_3dgs")) std::cerr << "Failed to save 3DGS\n";
        else std::cout << "Successfully saved. Use 'osgVerse_Test_3DGS da3_gs_result.splat.90,0,0.rot' to render it.\n";
    }
    else if (arguments.read("--free-splatter", modelFile))
    {
        std::vector<osg::ref_ptr<osg::Image>> images = obtainImages(dataLocation, true);
        if (images.empty()) { std::cerr << "No input image\n"; return 1; }

        osg::ref_ptr<osgVerse::FreeSplatter> fr = new osgVerse::FreeSplatter(modelFile, "gpu");
        loadedT = osg::Timer::instance()->tick();
        osgVerse::FreeSplatter::GaussianContent content = fr->estimateGaussians(images);
        endT = osg::Timer::instance()->tick();

        if (!content.positions) { std::cerr << "Failed to reconstruct 3DGS\n"; return 1; }
        //osgVerse::FreeSplatter::writeSplat(content, "free_out.splat");
        osg::ref_ptr<osgVerse::GaussianGeometry> geom = new osgVerse::GaussianGeometry;
        geom->setPosition(content.positions.get());
        geom->setScaleAndRotation(content.scales.get(), content.rotations.get(), content.alphas.get());
        geom->setShRed(0, content.reds.get()); geom->setShGreen(0, content.greens.get());
        geom->setShBlue(0, content.blues.get()); geom->setShDegrees(0);

        osg::ref_ptr<osg::Geode> g = new osg::Geode; g->addDrawable(geom.get());
        if (!osgDB::writeNodeFile(*g, "free_gs_result.splat.verse_3dgs")) std::cerr << "Failed to save 3DGS\n";
        else std::cout << "Successfully saved. Use 'osgVerse_Test_3DGS free_gs_result.splat.-90,0,0.rot' to render it.\n";
    }
    else if (arguments.read("--skin-tokens", modelFile))
    {   // SkinTokens based automatic rigging and skinning
        // Command-line: --skin-tokens <model_folder>
        //               --character ../models/Characters/CesiumMan.glb
        //               --bend-joint 6 --bend-angle 60 # an example to bend the arm
        if (characterFile.empty())
        { std::cerr << "Use '--character <model>' to give a static character model\n"; return 1; }

        // The glTF/FBX plugin can skip character data entirely, so that the input is a plain
        // static mesh which is exactly what the auto-rigging pipeline expects
        osg::ref_ptr<osgDB::Options> opt = new osgDB::Options;
        opt->setPluginStringData("DisabledSkinning", "1");
        osg::ref_ptr<osg::Node> loadedNode = osgDB::readNodeFile(characterFile, opt.get());
        if (!loadedNode.valid()) { std::cerr << "Failed to load character " << characterFile << "\n"; return 1; }

        // Wrap the loaded model in an identity group, so that the generated character lives in
        // a Z-up local space like the rest of the scene (the loaders keep Y-up content inside
        // their own root transformation)
        osg::ref_ptr<osg::Group> characterGroup = new osg::Group;
        characterGroup->addChild(loadedNode.get());
        characterNode = characterGroup;

        osg::ref_ptr<osgVerse::SkinTokens> engine = deviceName.empty()
            ? new osgVerse::SkinTokens(modelFile) : new osgVerse::SkinTokens(modelFile, deviceName);
        loadedT = osg::Timer::instance()->tick();

        osgVerse::SkinTokens::ModelInformation info;
        if (!engine->checkModelInformation(info))
        { std::cerr << "Failed to load SkinTokens model: " << engine->getLastError() << "\n"; return 1; }
        std::cout << "SkinTokens backend = " << info.backend << "\n";

        osg::ref_ptr<osgVerse::SkinTokens::RigResult> result = engine->generateRig(*characterNode);
        endT = osg::Timer::instance()->tick();
        if (!result.valid())
        { std::cerr << "Failed to generate rig: " << engine->getLastError() << "\n"; return 1; }

        std::cout << "Rig: " << result->joints.size() << " joints, " << result->vertexCount
                  << " vertices, " << result->triangleCount << " triangles, "
                  << (result->learned ? "learned weights" : "geometric baseline") << "\n";
        for (size_t i = 0; i < result->joints.size(); ++i)
        {
            const osgVerse::PlayerAnimation::JointBinding& jb = result->joints[i];
            std::cout << "  " << i << ": " << jb.name << ", parent = " << jb.parent
                      << ", pos = " << jb.translation << "\n";
        }

        characterPlayer = engine->createPlayer(*characterNode, *result);
        if (!characterPlayer.valid())
        { std::cerr << "Failed to create player: " << engine->getLastError() << "\n"; return 1; }

        if (!rigSaveFile.empty())
            std::cout << (engine->saveGLB(*result, rigSaveFile) ? "Saved rig to " : "Failed to save ")
                      << rigSaveFile << "\n";
        root->addChild(characterNode.get());

        // Bend one joint chain at runtime, which proves that the generated weights really
        // follow the skeleton (and is not only correct at the rest pose)
        if (bendJoint >= 0 && bendJoint < (int)result->joints.size())
        {
            characterPlayer->setSkeletonDrivenExternally(true);
            std::vector<osg::Matrix> rest = characterPlayer->getSkeletonRestPoseMatrices();
            std::vector<osgVerse::PlayerAnimation::ThisAndParent> indices =
                characterPlayer->getSkeletonIndices();
            osg::Vec3 pivot = rest[bendJoint].getTrans();
            osg::Matrix delta = osg::Matrix::translate(-pivot) * osg::Matrix::rotate(
                osg::DegreesToRadians(bendAngle), osg::Y_AXIS) * osg::Matrix::translate(pivot);
            for (size_t j = 0; j < rest.size() && j < indices.size(); ++j)
            {
                bool inSubtree = false; int current = (int)j;
                while (current >= 0 && current < (int)indices.size())
                {
                    if (current == bendJoint) { inSubtree = true; break; }
                    current = indices[current].second;
                }
                characterPlayer->setModelSpaceJointMatrix(
                    (int)j, inSubtree ? (rest[j] * delta) : rest[j]);
            }
            std::cout << "Bending joint " << bendJoint << " (" << result->joints[bendJoint].name
                      << ") by " << bendAngle << " degrees\n";
        }
    }
    else
        { arguments.getApplicationUsage()->write(std::cout); return 1; }

    std::cout << "Model load time: " << osg::Timer::instance()->delta_m(startT, loadedT) << "ms; ";
    std::cout << "Inference time: " << osg::Timer::instance()->delta_m(loadedT, endT) << "ms\n";
    if (root->getNumChildren() > 0)
    {
        viewer.addEventHandler(new osgViewer::StatsHandler);
        viewer.addEventHandler(new osgViewer::WindowSizeHandler);

        osg::ref_ptr<osgGA::CameraManipulator> manip = new osgGA::TrackballManipulator;
        if (characterNode.valid())
        {   // Characters are Z-up in osgVerse, so use a matching view facing the character
            osg::BoundingSphere bs = characterNode->getBound();
            osg::Vec3 center = bs.center(), up = osg::Z_AXIS;
            osg::Vec3 eye = center + osg::Vec3(0.0f, -bs.radius() * 2.8f, bs.radius() * 0.2f);
            manip->setHomePosition(eye, center, up);

            // In automation mode the view may already be set by --agent-eye; a manipulator is
            // not installed then either, so the requested eye is never replaced
            if (!agentMode) viewer.getCamera()->setViewMatrixAsLookAt(eye, center, up);
            else if (viewer.getCamera()->getViewMatrix().isIdentity())
                viewer.getCamera()->setViewMatrixAsLookAt(eye, center, up);
        }
        if (!agentMode) viewer.setCameraManipulator(manip.get());
        viewer.setSceneData(root.get());
        viewer.setUpViewInWindow(50, 50, 960, 600);
        return viewer.run();
    }
    return 0;
}
