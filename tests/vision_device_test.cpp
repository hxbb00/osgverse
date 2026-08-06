#include <osg/io_utils>
#include <osg/Texture2D>
#include <osg/PagedLOD>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <readerwriter/VisionDevice.h>
#include <readerwriter/Utilities.h>
#include <pipeline/Utilities.h>
#include <iostream>
#include <sstream>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    
    std::string devName; arguments.read("--device", devName);
    std::string options; arguments.read("-O", options);
    if (devName.empty() && arguments.argc() > 1) devName = arguments[1];

    osg::ref_ptr<osgVerse::VisionInputDevice> visionDev =
        dynamic_cast<osgVerse::VisionInputDevice*>(osgDB::readObjectFile(devName, new osgDB::Options(options)));
    if (!visionDev) { OSG_WARN << "Failed to load vision device " << devName << "\n"; return 1; }

    switch (visionDev->getState())
    {
    case osgVerse::VisionInputDevice::DeviceState::Connected:
        OSG_NOTICE << "Vision device connected: " << visionDev->getDeviceClassName() << ", Model "
                   << visionDev->getDeviceModelName() << " (SN = " << visionDev->getSerialNumber() << ")\n"; break;
    case osgVerse::VisionInputDevice::DeviceState::Error: OSG_NOTICE << "Vision device error!\n"; break;
    default: OSG_NOTICE << "Vision device not found or connected!\n"; break;
    }

    // Set up scene graph
    osg::ref_ptr<osg::Texture2D> visionTexture = new osg::Texture2D;
    visionTexture->setResizeNonPowerOfTwoHint(false);
    visionTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    visionTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    
    osg::Geometry* quad = osg::createTexturedQuadGeometry(
        osg::Vec3(), osg::X_AXIS * 1.6f, osg::Z_AXIS * 0.9f, 0.0f, 1.0f, 1.0f, 0.0f);
    quad->getOrCreateStateSet()->setTextureAttributeAndModes(0, visionTexture.get());

    osg::ref_ptr<osg::Geode> geode = new osg::Geode; geode->addDrawable(quad);
    osg::ref_ptr<osg::Group> root = new osg::Group; root->addChild(geode.get());

    // Start streaming
    visionDev->setEventCallback([&](osgVerse::VisionInputDevice::StreamEvent* ev)
        {
            if (ev->type == osgVerse::VisionInputDevice::StreamType::RGB && ev->image.valid())
            {
                if (visionTexture->getImage() != ev->image->image)
                    visionTexture->setImage(ev->image->image.get());
            }
        });
    visionDev->startStream(osgVerse::VisionInputDevice::StreamType::All);
    switch (visionDev->getState())
    {
    case osgVerse::VisionInputDevice::DeviceState::Streaming: OSG_NOTICE << "Vision device started streaming!\n"; break;
    default: OSG_NOTICE << "Vision device failed for some reason!\n"; break;
    }

    // Start the main loop
    osgViewer::Viewer viewer;
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    viewer.run();

    // Stop device running
    visionDev->disconnect();
    return 0;
}
