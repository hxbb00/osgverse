#ifndef VERSE_STRIKE_ARENA_H
#define VERSE_STRIKE_ARENA_H

#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osg/Math>
#include <osgDB/ReadFile>
#include <animation/Utilities.h>   // PhysicsUpdateCallback
#include <modeling/Utilities.h>    // createPrism / createPyramid
#include <pipeline/Utilities.h>    // createTexture2D / default textures / applyTextureAttributeAndModes
#include "GameContext.h"
#include "GamePaths.h"

namespace game
{
    static const float ARENA_HALF = 20.0f;

    /** Load a Kenney prototype grid texture from the fetched asset pack (may be NULL) */
    inline osg::Texture2D* loadPackTexture(const std::string& color, int index)
    {
        static std::string packDir = findGameAssetsDir();
        if (packDir.empty()) return NULL;
        std::string file = packDir + "/PrototypeTextures/PNG/" + color + "/texture_"
                         + (index < 10 ? "0" : "") + std::to_string(index) + ".png";
        osg::ref_ptr<osg::Image> img = osgDB::readRefImageFile(file);
        if (!img) return NULL;
        return osgVerse::createTexture2D(img.get(), osg::Texture::REPEAT);
    }

    /** Full material stateset for the deferred pipeline: our diffuse map (or the
        object color when the pack is absent) plus neutral defaults for the rest */
    inline void applyArenaMaterial(osg::Geode* geode, osg::Texture2D* diffuse, const osg::Vec4& color)
    {
        osg::StateSet* ss = geode->getOrCreateStateSet();
        osg::Vec4 black(0.0f, 0.0f, 0.0f, 0.0f), white(1.0f, 1.0f, 1.0f, 1.0f), orm(1.0f, 1.0f, 0.0f, 0.0f);
        osg::ref_ptr<osg::Texture2D> whiteTex = osgVerse::createDefaultTexture(white);
        osg::ref_ptr<osg::Texture2D> blackTex = osgVerse::createDefaultTexture(black);
        osg::ref_ptr<osg::Texture2D> ormTex = osgVerse::createDefaultTexture(orm);
        osgVerse::applyTextureAttributeAndModes(ss, 0, diffuse ? diffuse : whiteTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 1, blackTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 2, whiteTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 3, ormTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 4, blackTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 5, blackTex.get());
        osgVerse::applyTextureAttributeAndModes(ss, 6, blackTex.get());
        for (int i = 0; i < 7; ++i)
            ss->addUniform(new osg::Uniform(osgVerse::uniformNames[i].c_str(), i));
        for (unsigned int i = 0; i < geode->getNumDrawables(); ++i)
            static_cast<osg::ShapeDrawable*>(geode->getDrawable(i))->setColor(color);
    }

    inline osg::Geode* makeShapeGeode(osg::Shape* shape, const osg::Vec4& color,
                                      osg::Texture2D* tex = NULL)
    {
        osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(shape);
        sd->setColor(color);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(sd.get());
        if (tex) applyArenaMaterial(geode.get(), tex, color);
        return geode.release();
    }

    /** Static box (mass = 0): ground, walls, cover blocks */
    inline void addStaticBox(osgVerse::PhysicsEngine* physics, osg::Group* root,
                             const std::string& name, const osg::Vec3& halfSize,
                             const osg::Matrix& matrix, const osg::Vec4& color,
                             osg::Texture2D* tex = NULL)
    {
        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->setMatrix(matrix);
        mt->addChild(makeShapeGeode(
            new osg::Box(osg::Vec3(), halfSize.x() * 2, halfSize.y() * 2, halfSize.z() * 2),
            color, tex));
        root->addChild(mt.get());
        physics->addRigidBody(name, physics->createPhysicsBox(halfSize), 0.0f, matrix);
    }

    /** Dynamic physics crate which follows its rigid body automatically */
    inline ecs::Entity spawnCrate(GameContext& ctx, const osg::Vec3& pos,
                                  float half, float mass, const osg::Vec4& color,
                                  osg::Texture2D* tex = NULL)
    {
        static int s_crateCount = 0;
        std::string bodyName = "crate_" + std::to_string(++s_crateCount);

        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->setMatrix(osg::Matrix::translate(pos));
        mt->addChild(makeShapeGeode(
            new osg::Box(osg::Vec3(), half * 2, half * 2, half * 2), color, tex));
        ctx.sceneRoot->addChild(mt.get());

        ctx.physics->addRigidBody(bodyName,
            ctx.physics->createPhysicsBox(osg::Vec3(half, half, half)), mass, mt->getMatrix());
        mt->setUpdateCallback(new osgVerse::PhysicsUpdateCallback(ctx.physics, bodyName));

        ecs::Entity e = ctx.registry->create();
        ctx.registry->emplace<NodeLink>(e, mt.get());
        ctx.registry->emplace<BodyLink>(e, bodyName);
        return e;
    }

    /** Shooting range dummy: cylinder body + sphere head, respawns when killed */
    inline ecs::Entity spawnTarget(GameContext& ctx, const osg::Vec3& pos)
    {
        static int s_targetCount = 0;
        std::string bodyName = "target_" + std::to_string(++s_targetCount);

        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->setMatrix(osg::Matrix::translate(pos));
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(new osg::ShapeDrawable(
                new osg::Cylinder(osg::Vec3(0.0f, 0.0f, 0.9f), 0.3f, 1.8f)));
            geode->addDrawable(new osg::ShapeDrawable(
                new osg::Sphere(osg::Vec3(0.0f, 0.0f, 1.95f), 0.22f)));
            ctx.sceneRoot->addChild(mt.get());
            for (unsigned int i = 0; i < geode->getNumDrawables(); ++i)
                static_cast<osg::ShapeDrawable*>(geode->getDrawable(i))->setColor(
                    osg::Vec4(0.85f, 0.35f, 0.2f, 1.0f));
            mt->addChild(geode.get());
        }

        osg::Matrix bodyMatrix = osg::Matrix::translate(pos + osg::Vec3(0.0f, 0.0f, 0.9f));
        ctx.physics->addRigidBody(bodyName,
            ctx.physics->createPhysicsCylinder(osg::Vec3(0.3f, 0.3f, 0.9f)), 0.0f, bodyMatrix);

        ecs::Entity e = ctx.registry->create();
        ctx.registry->emplace<NodeLink>(e, mt.get());
        ctx.registry->emplace<BodyLink>(e, bodyName);
        ctx.registry->emplace<Health>(e, 3.0f);
        TargetTag tag; tag.spawnPose = bodyMatrix;
        ctx.registry->emplace<TargetTag>(e, tag);
        return e;
    }

    /** Small first-person weapon viewmodel, placed by updatePlayer() each frame */
    inline osg::MatrixTransform* createGun()
    {
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        osg::ref_ptr<osg::ShapeDrawable> body = new osg::ShapeDrawable(
            new osg::Box(osg::Vec3(0.0f, 0.0f, 0.0f), 0.08f, 0.06f, 0.34f));
        body->setColor(osg::Vec4(0.15f, 0.15f, 0.18f, 1.0f));
        osg::ref_ptr<osg::ShapeDrawable> barrel = new osg::ShapeDrawable(
            new osg::Cylinder(osg::Vec3(0.0f, 0.0f, -0.26f), 0.022f, 0.28f));
        barrel->setColor(osg::Vec4(0.08f, 0.08f, 0.09f, 1.0f));
        osg::ref_ptr<osg::ShapeDrawable> grip = new osg::ShapeDrawable(
            new osg::Box(osg::Vec3(0.0f, -0.08f, 0.16f), 0.06f, 0.05f, 0.14f));
        grip->setColor(osg::Vec4(0.2f, 0.14f, 0.08f, 1.0f));
        geode->addDrawable(body.get());
        geode->addDrawable(barrel.get());
        geode->addDrawable(grip.get());

        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->setName("Gun");
        mt->addChild(geode.get());
        return mt.release();
    }

    /** The procedural arena: floor, perimeter walls, cover blocks, crates and targets */
    inline void buildArena(GameContext& ctx)
    {
        const float H = ARENA_HALF;
        osg::Vec4 groundColor(0.42f, 0.44f, 0.48f, 1.0f);
        osg::Vec4 wallColor(0.55f, 0.52f, 0.45f, 1.0f);
        osg::Vec4 coverColor(0.72f, 0.68f, 0.50f, 1.0f);
        osg::Vec4 crateColor(0.85f, 0.75f, 0.30f, 1.0f);

        // Kenney prototype grid textures (CC0) when the asset pack was fetched;
        // the ShapeDrawable colors tint the grayscale grid, or stand alone otherwise
        // Use the 32bpp "Light" tiles for large surfaces: indexed 4bpp variants can
        // lose their alpha when loaded, and the GBuffer discards low-alpha fragments
        osg::ref_ptr<osg::Texture2D> texGround = loadPackTexture("Light", 2);
        osg::ref_ptr<osg::Texture2D> texWall = loadPackTexture("Light", 2);
        osg::ref_ptr<osg::Texture2D> texCover = loadPackTexture("Green", 1);
        osg::ref_ptr<osg::Texture2D> texCrate = loadPackTexture("Orange", 1);
        if (texGround.valid()) OSG_NOTICE << "[GameStrike] Using prototype textures from the asset pack\n";

        addStaticBox(ctx.physics, ctx.sceneRoot, "ground", osg::Vec3(H, H, 0.1f),
                     osg::Matrix::translate(0.0f, 0.0f, -0.1f), groundColor, texGround.get());

        addStaticBox(ctx.physics, ctx.sceneRoot, "wall_south", osg::Vec3(H, 0.3f, 2.0f),
                     osg::Matrix::translate(0.0f, -H, 2.0f), wallColor, texWall.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "wall_north", osg::Vec3(H, 0.3f, 2.0f),
                     osg::Matrix::translate(0.0f, H, 2.0f), wallColor, texWall.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "wall_west", osg::Vec3(0.3f, H, 2.0f),
                     osg::Matrix::translate(-H, 0.0f, 2.0f), wallColor, texWall.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "wall_east", osg::Vec3(0.3f, H, 2.0f),
                     osg::Matrix::translate(H, 0.0f, 2.0f), wallColor, texWall.get());

        // Cover blocks (asymmetric layout so shadows/AO are visible from anywhere)
        addStaticBox(ctx.physics, ctx.sceneRoot, "cover_1", osg::Vec3(2.0f, 0.5f, 1.0f),
                     osg::Matrix::translate(6.0f, 4.0f, 1.0f), coverColor, texCover.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "cover_2", osg::Vec3(0.5f, 2.0f, 1.0f),
                     osg::Matrix::translate(-7.0f, -2.0f, 1.0f), coverColor, texCover.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "cover_3", osg::Vec3(1.2f, 1.2f, 1.2f),
                     osg::Matrix::translate(-3.0f, 10.0f, 1.2f), coverColor, texCover.get());
        addStaticBox(ctx.physics, ctx.sceneRoot, "cover_4", osg::Vec3(1.5f, 0.4f, 0.8f),
                     osg::Matrix::translate(10.0f, -8.0f, 0.8f), coverColor, texCover.get());

        // Watchtower: stepping platforms up to a sniper ledge
        for (int i = 0; i < 3; ++i)
        {
            addStaticBox(ctx.physics, ctx.sceneRoot, "step_" + std::to_string(i),
                         osg::Vec3(1.2f, 1.2f, 0.4f * (i + 1)),
                         osg::Matrix::translate(-10.0f + 2.0f * i, -12.0f, 0.4f * (i + 1)),
                         coverColor, texCover.get());
        }
        addStaticBox(ctx.physics, ctx.sceneRoot, "ledge", osg::Vec3(2.5f, 1.5f, 0.2f),
                     osg::Matrix::translate(-6.0f, -12.0f, 1.6f), coverColor, texCover.get());

        // Some dynamic crates to push/shoot around
        for (int i = 0; i < 6; ++i)
        {
            spawnCrate(ctx, osg::Vec3(-6.0f + i * 2.4f, 8.0f, 0.5f), 0.5f, 5.0f,
                       crateColor, texCrate.get());
        }

        // Targets scattered around the arena
        spawnTarget(ctx, osg::Vec3(-12.0f, 6.0f, 0.0f));
        spawnTarget(ctx, osg::Vec3(0.0f, 14.0f, 0.0f));
        spawnTarget(ctx, osg::Vec3(12.0f, 8.0f, 0.0f));
        spawnTarget(ctx, osg::Vec3(14.0f, -10.0f, 0.0f));
        spawnTarget(ctx, osg::Vec3(-14.0f, -6.0f, 0.0f));
        spawnTarget(ctx, osg::Vec3(-6.0f, -12.0f, 3.6f));  // one on the ledge
    }
}

#endif
