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
#include <ai/TrellisPipeline.h>
#include <ai/Utilities.h>
#include <modeling/GaussianGeometry.h>
#include <modeling/Utilities.h>
#include <pipeline/Drawer2D.h>
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
    else
        { arguments.getApplicationUsage()->write(std::cout); return 1; }

    std::cout << "Model load time: " << osg::Timer::instance()->delta_m(startT, loadedT) << "ms; ";
    std::cout << "Inference time: " << osg::Timer::instance()->delta_m(loadedT, endT) << "ms\n";
    if (root->getNumChildren() > 0)
    {
        osgViewer::Viewer viewer;
        viewer.addEventHandler(new osgViewer::StatsHandler);
        viewer.addEventHandler(new osgViewer::WindowSizeHandler);
        viewer.setCameraManipulator(new osgGA::TrackballManipulator);
        viewer.setSceneData(root.get());
        viewer.setUpViewInWindow(50, 50, 960, 600);
        return viewer.run();
    }
    return 0;
}
