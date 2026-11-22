#include PREPENDED_HEADER
#include <osg/io_utils>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgViewer/ViewerEventHandlers>

#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <animation/PhysicsEngine.h>

#include "Ecs.h"
#include "GameContext.h"
#include "Arena.h"
#include "Systems.h"

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
#endif

using namespace game;

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments =
        osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler);

    // Physics world: the default Box3D core; --bullet switches to the Bullet plugin
    osg::ref_ptr<osgVerse::PhysicsEngine> physics;
    if (arguments.read("--bullet"))
        physics = dynamic_cast<osgVerse::PhysicsEngine*>(osgDB::readObjectFile("0.verse_bullet"));
    if (!physics) physics = new osgVerse::PhysicsEngine;
    OSG_NOTICE << "[GameStrike] Physics engine: " << physics->getName() << "\n";

    // ECS registry + scene graph
    ecs::Registry registry;
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->setName("GameStrikeRoot");
    osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
    sceneRoot->setName("SceneRoot");
    root->addChild(sceneRoot.get());
    osgVerse::Pipeline::setPipelineMask(*sceneRoot,
        DEFERRED_SCENE_MASK | SHADOW_CASTER_MASK);

    // Viewer & standard pipeline (light/skybox/shadows are set up by realize())
    osgVerse::StandardPipelineViewer viewer;
    viewer.setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
    viewer.setSceneData(root.get());

    // Game world
    GameContext ctx;
    ctx.physics = physics.get();
    ctx.registry = &registry;
    ctx.sceneRoot = sceneRoot.get();
    buildArena(ctx);
    ctx.gunNode = createGun();
    sceneRoot->addChild(ctx.gunNode.get());

    // Player
    ctx.character = new osgVerse::PhysicsCharacter(physics.get(), "player");
    if (!ctx.character->create(osg::Vec3(0.0f, -8.0f, 1.0f)))
    { OSG_WARN << "[GameStrike] Failed to create the player\n"; return 1; }

    // Effects, HUD, input
    ctx.effects = new Effects(&registry, root.get());
    ctx.hud = new Hud(root.get());
    ctx.input = new FpsController;
    viewer.addEventHandler(ctx.input);
    viewer.addEventHandler(new osgViewer::StatsHandler);

    viewer.setUpViewOnSingleScreen(0);
    viewer.realize();
    osgVerse::configureViewerForAgent(arguments, &viewer);

    // Camera projection for a first-person view
    osg::Camera* camera = viewer.getCamera();
    double fov, aspect, nearZ, farZ;
    camera->getProjectionMatrixAsPerspective(fov, aspect, nearZ, farZ);
    camera->setProjectionMatrixAsPerspective(60.0, aspect, 0.1, 250.0);
    ctx.camera = camera;
    ctx.input->applyCamera(camera, ctx.character->getPosition() + osg::Vec3(0.0f, 0.0f, 0.55f));

    double lastT = 0.016;
    while (!viewer.done())
    {
        double t = viewer.getFrameStamp()->getSimulationTime();
        double dt = (t > lastT) ? (t - lastT) : 0.016;
        lastT = t;

        updatePlayer(ctx, dt);
        updateCombat(ctx);
        updateTargets(ctx, dt);
        updateEffects(ctx, dt);
        updateHud(ctx, dt);
        viewer.frame();
    }
    return 0;
}
