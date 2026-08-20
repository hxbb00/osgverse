#include <osg/io_utils>
#include <osg/Texture2D>
#include <osg/Depth>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgText/Font>
#include <osgText/Text>
#include <osgGA/TrackballManipulator>
#include <osgGA/StateSetManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <modeling/AnnotationCluster.h>
#include <modeling/Math.h>
#include <pipeline/Utilities.h>
#include <readerwriter/EarthManipulator.h>
#include <readerwriter/FeatureDefinition.h>
#include <readerwriter/Utilities.h>
#include <iostream>
#include <sstream>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    std::string file(BASE_DIR + "/models/VectorData/WorldCities.fgb.verse_geojson");
    arguments.read("--file", file);

    // Load POI features
    osg::ref_ptr<osgVerse::FeatureCollection> fc =
        dynamic_cast<osgVerse::FeatureCollection*>(osgDB::readObjectFile(file));
    if (!fc) { OSG_WARN << "No features read from: " << file << std::endl; return 1;  }

    osg::ref_ptr<osg::Group> root = new osg::Group;
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
    root->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("BaseTexture"));
    root->getOrCreateStateSet()->addUniform(new osg::Uniform("BaseTexture", (int)0));
#else
    root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif

    osg::ref_ptr<osgText::Font> font = osgText::readFontFile(MISC_DIR + "LXGWFasmartGothic.ttf");
    osg::ref_ptr<osgVerse::AnnotationCluster> cluster = new osgVerse::AnnotationCluster;
    //cluster->setClusterOptions(40.0f, 7);
    cluster->setConvertCellToLatLon(false);  // set false to input (lat, lon, 0) in radians directly
    cluster->setClusterGroupFunc([font](const osg::Vec3d& pos, unsigned int clusterID, unsigned int count) -> osg::Node*
    {
        osg::Vec3d ecef = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(pos[0], pos[1], 1000.0));
        osg::ref_ptr<osgText::Text> textObj = new osgText::Text;
        textObj->setPosition(ecef); textObj->setText(std::to_string(count));
        textObj->setAlignment(osgText::Text::CENTER_CENTER);
        textObj->setCharacterSizeMode(osgText::Text::OBJECT_COORDS_WITH_MAXIMUM_SCREEN_SIZE_CAPPED_BY_FONT_HEIGHT);
        textObj->setCharacterSize(osg::minimum(4800.0f * count, (float)40e4), 1.0f);
        textObj->setAxisAlignment(osgText::Text::SCREEN);
        textObj->setFont(font.get());
        
        osg::ref_ptr<osg::Geode> textGeode = new osg::Geode;
        textGeode->addDrawable(textObj.get()); return textGeode.release();
    });
    root->addChild(cluster->getClusterNode());
    
    for (size_t i = 0; i < fc->features.size(); ++i)
    {
        osgVerse::Feature* feature = fc->features[i];
        std::string name; osg::Vec3d lonLat = feature->getBound().center();
        if (feature->getUserValue("NAME", name))
        {
            osg::Vec3d latLon(osg::inDegrees(lonLat[1]), osg::inDegrees(lonLat[0]), 1000.0);
            osg::Vec3d ecef = osgVerse::Coordinate::convertLLAtoECEF(latLon);
            
            osg::ref_ptr<osgText::Text> textObj = new osgText::Text;
            textObj->setPosition(ecef); textObj->setText(name);
            textObj->setAlignment(osgText::Text::CENTER_CENTER);
            textObj->setCharacterSizeMode(osgText::Text::OBJECT_COORDS_WITH_MAXIMUM_SCREEN_SIZE_CAPPED_BY_FONT_HEIGHT);
            textObj->setAxisAlignment(osgText::Text::SCREEN);
            textObj->setCharacterSize(12000.0f, 1.0f);
            textObj->setFont(font.get());
            
            osg::ref_ptr<osg::Geode> textGeode = new osg::Geode;
            textGeode->addDrawable(textObj.get()); root->addChild(textGeode.get());
            cluster->add(latLon, textGeode.get(), name);
        }
    }

    // Load default earth
    std::string mainFolder = BASE_DIR + "/models/Earth"; arguments.read("--folder", mainFolder);
    std::string earthURLs = " Orthophoto=mbtiles://" + mainFolder + "/DOM_lv4.mbtiles/{z}-{x}-{y}.jpg"
                            " UseWebMercator=1 UseEarth3D=1 OriginBottomLeft=1 TileElevationScale=3 TileSkirtRatio=0.05";
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options(earthURLs);
    osg::ref_ptr<osg::Node> earth = osgDB::readNodeFile("0-0-0.verse_tms", options.get());
    
    osgViewer::Viewer viewer;
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    if (earth.valid())
    {
        osg::ref_ptr<osgVerse::EarthManipulator> manipulator = new osgVerse::EarthManipulator;
        //manipulator->setWorldNode(earth.get()); viewer.setCameraManipulator(manipulator.get());
        root->addChild(earth.get());
    }

    // Start the main loop
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));
    viewer.setSceneData(root.get());
    viewer.setUpViewOnSingleScreen(0);
    while (!viewer.done())
    {
        cluster->update(viewer.getCamera(), 10000.0);
        viewer.frame();
    }
    return 0;
}
