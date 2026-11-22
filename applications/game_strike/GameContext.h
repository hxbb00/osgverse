#ifndef VERSE_STRIKE_GAMECONTEXT_H
#define VERSE_STRIKE_GAMECONTEXT_H

#include <osg/Group>
#include <osg/Camera>
#include <animation/PhysicsEngine.h>
#include "Ecs.h"
#include "Components.h"
#include "FpsController.h"
#include "Effects.h"
#include "Hud.h"

namespace game
{
    /** Non-owning bundle of engine references shared by all game systems */
    struct GameContext
    {
        osgVerse::PhysicsEngine* physics;
        ecs::Registry* registry;
        osg::Group* sceneRoot;   // world entities (deferred pass)
        osg::Camera* camera;
        FpsController* input;
        Effects* effects;
        Hud* hud;
        osgVerse::PhysicsCharacter* character;
        osg::ref_ptr<osg::MatrixTransform> gunNode;
        int score;
        float time;

        GameContext()
          : physics(NULL), registry(NULL), sceneRoot(NULL), camera(NULL),
            input(NULL), effects(NULL), hud(NULL), character(NULL),
            score(0), time(0.0f) {}
    };
}

#endif
