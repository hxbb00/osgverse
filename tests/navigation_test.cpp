#include <osg/io_utils>
#include <osg/LightSource>
#include <osg/Texture2D>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <iostream>
#include <sstream>

#include <VerseCommon.h>
#include <pipeline/IntersectionManager.h>
#include <pipeline/Pipeline.h>
#include <ui/Utilities.h>
#include <ai/RecastManager.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

class InteractiveHandler : public osgGA::GUIEventHandler
{
public:
    typedef osgVerse::HeadUpDisplayCanvas::ChildLayout ChildLayout;
    typedef osgVerse::HeadUpDisplayCanvas::Anchor Anchor;
    InteractiveHandler(osg::Group* root, osgVerse::HeadUpDisplayCanvas* h, osg::Node* ag, osgVerse::RecastManager* rm)
        : _agentNode(ag), _canvas(h), _root(root), _recast(rm), _agentID(0)
    {
        _axesNode = osgDB::readNodeFile("axes.osgt.5,5,5.scale");
        _canvas->createText("main", L"Alt+click to create new agent",
                            32, 800, 40, "root", ChildLayout::FREE, Anchor::BOTTOM);
        _canvas->createText("event", L"", 32, 800, 40, "root", ChildLayout::FREE, Anchor::TOP);
        _canvas->createText("button", L"Cancel", 16, 100, 40, "root", ChildLayout::FREE, Anchor::BOTTOM | Anchor::RIGHT);
        _canvas->setAsButton("button", [&]() { select(NULL); });
    }

    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
        if (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE &&
            (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL))
        {
            osgVerse::IntersectionResult result = osgVerse::findNearestIntersection(
                view->getCamera(), ea.getXnormalized(), ea.getYnormalized());
            if (!result.drawable) return false;

            osg::Node* agent = result.findNode([](osg::Node* node)
            { return node->getName().find("RecastAgent") != std::string::npos; });
            select(agent ? agent->asGroup() : NULL);
        }
        else if (ea.getEventType() == osgGA::GUIEventAdapter::DOUBLECLICK ||
                 (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE && (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_ALT)))
        {
            osgVerse::IntersectionResult result = osgVerse::findNearestIntersection(
                view->getCamera(), ea.getXnormalized(), ea.getYnormalized());
            std::stringstream ss; if (!result.drawable) return false;

            if (_selectedAgent != NULL)
            {
                osg::Node* agent = result.findNode([](osg::Node* node)
                { if (node->getName().find("RecastAgent") != std::string::npos) return true; else return false; });
                if (agent != NULL) return false;  // nothing to do

                // Select a new target for position current agent
                osgVerse::RecastManager::Agent* aData =
                    static_cast<osgVerse::RecastManager::Agent*>(_recast->getAgentFromNode(_selectedAgent.get()));
                if (aData != NULL)
                {
                    aData->target = result.getWorldIntersectPoint(); _recast->updateAgent(aData);
                    ss << "Set new position of " << _selectedAgent->getName() << std::endl;
                    _canvas->texts["event"]->setText(ss.str());
                }
            }
            else  // click on ground to create new agent
            {
                osg::ref_ptr<osg::MatrixTransform> player = new osg::MatrixTransform;
                player->setMatrix(osg::Matrix::translate(result.getWorldIntersectPoint()));
                player->addChild(_agentNode.get()); player->setName("RecastAgent" + std::to_string(_agentID++));
                osgVerse::Pipeline::setPipelineMask(*player, DEFERRED_SCENE_MASK & (~SHADOW_CASTER_MASK));
                select(player.get()); _root->addChild(player.get());

                ss << "Created new agent " << player->getName() << std::endl;
                _canvas->texts["event"]->setText(ss.str());

                osg::ref_ptr<osgVerse::RecastManager::Agent> agent =
                    new osgVerse::RecastManager::Agent(player.get(), result.getWorldIntersectPoint());
                agent->maxSpeed = 5.0f; agent->maxAcceleration = 8.0f;
                _recast->updateAgent(agent.get());
            }
        }
        return false;
    }

    void select(osg::Group* agent)
    {
        if (_selectedAgent.valid()) _selectedAgent->removeChild(_axesNode.get());
        _selectedAgent = agent; if (_selectedAgent.valid()) _selectedAgent->addChild(_axesNode.get());

        if (agent)
        {
            _canvas->texts["event"]->setText("Selected the agent " + agent->getName());
            _canvas->texts["main"]->setText("Alt+click to set target position; "
                                            "or Ctrl+click to select another agent");
        }
        else
        {
            _canvas->texts["event"]->setText("");
            _canvas->texts["main"]->setText("Alt+click to create new agent; "
                                            "or Ctrl+click to select a agent");
        }
    }

protected:
    osg::ref_ptr<osg::Node> _agentNode, _axesNode;
    osg::observer_ptr<osg::Group> _root, _selectedAgent;
    osg::observer_ptr<osgVerse::RecastManager> _recast;
    osgVerse::HeadUpDisplayCanvas* _canvas;
    unsigned int _agentID;
};

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    std::string agentPath; arguments.read("--agent", agentPath);
    std::string recastData = "recast_terrain.bin"; arguments.read("--recast", recastData);
    osg::ref_ptr<osg::Node> terrain = osgDB::readNodeFiles(arguments);
    if (!terrain) terrain = osgDB::readNodeFile("lz.osg");

    osg::ref_ptr<osgVerse::RecastManager> recast = new osgVerse::RecastManager;
    osgVerse::RecastSettings settings = recast->getSettings();

    osg::ref_ptr<osg::Node> agentNode = agentPath.empty() ? NULL : osgDB::readNodeFile(agentPath);
    if (agentNode.valid())
    {
        settings.agentRadius = agentNode->getBound().radius();
        settings.agentHeight = settings.agentRadius * 2.0f;
        recast->setSettings(settings);
    }
    else
    {
        osg::ShapeDrawable* shape = new osg::ShapeDrawable(new osg::Cylinder(
            osg::Z_AXIS * settings.agentHeight, settings.agentRadius, settings.agentHeight));
        osg::Geode* geode = new osg::Geode; agentNode = geode;
        shape->setColor(osg::Vec4(1.0f, 1.0f, 0.0f, 1.0f)); geode->addDrawable(shape);
    }

    std::ifstream dataIn(recastData, std::ios::in | std::ios::binary);
    if (arguments.read("--no-preload") || !dataIn)
    {
        std::ofstream dataOut(recastData, std::ios::out | std::ios::binary);
        if (recast->build(terrain.get(), true)) recast->save(dataOut);
        else { OSG_WARN << "Failed to build nav-mesh." << std::endl; return -1; }
    }
    else
    { recast->read(dataIn); OSG_NOTICE << "Read recast data from file." << std::endl; }
    recast->initializeAgents();

    osg::ref_ptr<osg::MatrixTransform> debugNode = new osg::MatrixTransform;
    debugNode->addChild(recast->getDebugMesh());
    //debugNode->setMatrix(osg::Matrix::translate(0.0f, 0.0f, 1.0f));
    debugNode->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    debugNode->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
    debugNode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif
    debugNode->getOrCreateStateSet()->setMode(GL_DEPTH, osg::StateAttribute::OFF);

    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
    root->addChild(terrain.get()); root->addChild(debugNode.get());
    osgVerse::Pipeline::setPipelineMask(*terrain, DEFERRED_SCENE_MASK & (~SHADOW_CASTER_MASK));

    osg::Geode* geode = new osg::Geode;
    //geode->addDrawable(new osg::ShapeDrawable(
    //    osgVerse::createHeightField(terrain.get(), 4096, 4096)));
    //root->addChild(geode);

    osgVerse::HeadUpDisplayCanvas hudCanvas;
    root->addChild(hudCanvas.create(1920, 1080));

#if false
    osgVerse::StandardPipelineViewer viewer;
#else
    osgViewer::Viewer viewer;
#endif
    viewer.addEventHandler(new InteractiveHandler(root.get(), &hudCanvas, agentNode.get(), recast.get()));
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    while (!viewer.done())
    {
        recast->advance(viewer.getFrameStamp()->getSimulationTime(), 2.5f);
        viewer.frame();
    }
    return 0;
}
