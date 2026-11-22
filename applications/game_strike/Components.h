#ifndef VERSE_STRIKE_COMPONENTS_H
#define VERSE_STRIKE_COMPONENTS_H

#include <osg/MatrixTransform>
#include "Ecs.h"

namespace game
{
    /** Scene node of an entity, positioned by physics or by a system */
    struct NodeLink
    {
        osg::ref_ptr<osg::MatrixTransform> node;
        NodeLink() : node(NULL) {}
        NodeLink(osg::MatrixTransform* n) : node(n) {}
    };

    /** Physics rigid body of an entity */
    struct BodyLink { std::string name; };

    struct Health
    {
        float hp, maxHp;
        Health() : hp(0.0f), maxHp(0.0f) {}
        Health(float v) : hp(v), maxHp(v) {}
    };

    /** Destructible target: hides itself when killed and respawns after a delay */
    struct TargetTag
    {
        float respawnDelay, respawnTimer;
        osg::Matrix spawnPose;
        bool dead;
        TargetTag() : respawnDelay(3.0f), respawnTimer(0.0f), dead(false) {}
    };

    /** Transient effect node (e.g. tracers), removed when remaining <= 0 */
    struct Lifetime
    {
        float remaining;
        Lifetime(float t) : remaining(t) {}
    };
}

#endif
