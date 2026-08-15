#include <osg/io_utils>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <pipeline/ExternalTexture2D.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <readerwriter/GraphicsWindowSDL.h>
#include <readerwriter/GraphicsWindowGLFW.h>
#include <readerwriter/Utilities.h>
#include <iostream>
#include <sstream>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

static osgVerse::GraphicsWindowHandle* getEglHandle(osg::GraphicsContext* gc)
{
    osgVerse::GraphicsWindowSDL* sdl = dynamic_cast<osgVerse::GraphicsWindowSDL*>(gc);
    osgVerse::GraphicsWindowGLFW* glfw = dynamic_cast<osgVerse::GraphicsWindowGLFW*>(gc);
    if (sdl) return sdl->getHandle(); if (glfw) return glfw->getHandle(); return NULL;
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    std::string file; bool recordeMode = arguments.read("--record");
    std::string testImg; bool testMode = arguments.read("--test", testImg);
    if (!recordeMode && !testMode && !arguments.read("--file", file))
    {
        std::cout << "Please specify a movie file name or stream URL with --file."
                  << std::endl; return 1;
    }
    if (file.empty()) { file = "record.mp4.verse_ffmpeg"; recordeMode = true; }

    osg::ref_ptr<osgVerse::ExternalTexture2D> videoTexture;
    osg::ref_ptr<osgVerse::GpuResourceDemuxerMuxerContainer> videoRecorder;

    // Initialize viewer first to get possible EGL context data from GraphicsWindow
    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
    root->addChild(osgDB::readNodeFile("axes.osgt"));

    osgViewer::Viewer viewer;
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    viewer.setUpViewOnSingleScreen(0);

#ifdef VERSE_WITH_CUDA
    CUcontext inputContext = osgVerse::CudaAlgorithm::initializeContext(0);
#else
    osgVerse::GraphicsWindowHandle* H = getEglHandle(viewer.getCamera()->getGraphicsContext());
    if (!H || (H && !H->eglDisplay))
    {
        OSG_WARN << "No Cuda or EGL context found. No method to play video on GPU device." << std::endl;
        return 0;
    }
    void* inputContext = H->eglDisplay;
#endif

    // Test, encode or decode video
    osgDB::Options* opt = new osgDB::Options; opt->setPluginData("Context", inputContext);
    if (testMode)
    {   // Show a test image to see if external-texture can work
        osg::ref_ptr<osgVerse::GpuResourceReaderBase> defReader = new osgVerse::GpuResourceReaderBase(inputContext);
        defReader->setDefaultTestImage(osgDB::readImageFile(testImg));
        
        // Create the texture
        videoTexture = new osgVerse::ExternalTexture2D;
        videoTexture->setResourceReader(defReader.get());
        videoTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        videoTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    }
    else if (recordeMode)
    {   // Encode and output video
        osgVerse::GpuResourceReaderWriterContainer* container =
            dynamic_cast<osgVerse::GpuResourceReaderWriterContainer*>(osgDB::readObjectFile("encoder.codec_nv", opt));
        if (!container)
        {
            OSG_WARN << "No encoder found for video recording" << std::endl;
            return 0;
        }

        videoRecorder = new osgVerse::GpuResourceDemuxerMuxerContainer;
        container->getWriter()->openResource(videoRecorder.get());
        // Use osgDB::writeObjectFile(*videoRecorder, name) to create muxer and save H264 frames

        // Set up scene graph
        // TODO: add container1->getWriter() to camera drawcallback
    }
    else
    {   // Decode and play video
        osgVerse::GpuResourceReaderWriterContainer* container =
            dynamic_cast<osgVerse::GpuResourceReaderWriterContainer*>(osgDB::readObjectFile("decoder.codec_nv", opt));
        if (!container)
        {
            OSG_WARN << "No decoder found for video playing" << std::endl;
            return 0;
        }

        osgVerse::GpuResourceDemuxerMuxerContainer* videoReader =
            dynamic_cast<osgVerse::GpuResourceDemuxerMuxerContainer*>(osgDB::readObjectFile(file));
        if (!videoReader)
        {
            OSG_WARN << "No demuxer found for video file: " << file << std::endl;
            return 0;
        }

        // Create the texture
        videoTexture = new osgVerse::ExternalTexture2D;
        videoTexture->setResourceReader(container->getReader());
        if (!container->getReader()->openResource(videoReader))
            OSG_WARN << "Failed to open video demuxer: " << file << std::endl;
        videoTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        videoTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

        // Add audio container if needed
        container->getReader()->setAudioContainer(osgVerse::AudioPlayer::instance());
    }

    // Set up scene graph
    if (videoTexture.valid())
    {
        osg::Geometry* quad = osg::createTexturedQuadGeometry(
            osg::Vec3(), osg::X_AXIS * 1.6f, osg::Z_AXIS * 0.9f, 0.0f, 1.0f, 1.0f, 0.0f);
        quad->getOrCreateStateSet()->setTextureAttributeAndModes(0, videoTexture.get());

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(quad); root->addChild(geode.get());
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
        root->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        root->getOrCreateStateSet()->addUniform(new osg::Uniform("baseTexture", (int)0));
#endif
    }
    while (!viewer.done())
    {
        viewer.frame();
        if (videoRecorder.valid()) osgDB::writeObjectFile(*videoRecorder, file);
    }

    if (videoTexture.valid()) videoTexture->releaseGpuData();
    osgVerse::CudaAlgorithm::deinitializeContext(inputContext);
    return 0;
}
