#include "Systems.h"
#include <osg/Camera>
#include <osg/Matrix>
#include <osg/Vec3>

namespace game
{
    void updatePlayer(GameContext& ctx, double dt)
    {
        float dtf = (float)dt;
        if (dtf <= 0.0f) return;
        if (dtf > 0.05f) dtf = 0.05f;  // clamp against hitches

        ctx.character->setWishVelocity(
            ctx.input->wishDirection() * (ctx.input->isSprinting() ? 1.0f : 0.55f));
        if (ctx.input->consumeJump()) ctx.character->jump();

        ctx.character->step(dtf);
        ctx.physics->advance(dtf, 4);
        ctx.character->lateStep();

        osg::Vec3 eye = ctx.character->getPosition() + osg::Vec3(0.0f, 0.0f, 0.55f);
        ctx.input->applyCamera(ctx.camera, eye);

        // Weapon viewmodel: defined in camera space, converted to world each frame
        if (ctx.gunNode.valid())
        {
            osg::Matrix camWorld = osg::Matrix::inverse(ctx.camera->getViewMatrix());
            ctx.gunNode->setMatrix(osg::Matrix::translate(osg::Vec3(0.32f, -0.30f, -0.85f)) * camWorld);
        }
        ctx.time += dtf;
    }

    void updateCombat(GameContext& ctx)
    {
        if (!ctx.input->consumeFire()) return;
        ctx.time = ctx.time;  // fire rate limiting may be added here later

        osg::Vec3 eye, center, up;
        ctx.camera->getViewMatrixAsLookAt(eye, center, up);
        osg::Vec3 dir = ctx.input->viewDirection();
        osg::Vec3 muzzle = eye + dir * 0.7f;
        osg::Vec3 end = eye + dir * 100.0f;
        ctx.effects->flash(muzzle);

        osgVerse::PhysicsEngine::QueryFilter filter;
        filter.excludeNames.insert(ctx.character->getName());
        osgVerse::PhysicsEngine::RaycastHit hit;
        if (!ctx.physics->raycast(eye, end, hit, filter))
        {
            ctx.effects->tracer(muzzle, end);
            return;
        }

        ctx.effects->tracer(muzzle, hit.position);
        ctx.effects->flash(hit.position, 0.08f);

        ecs::Entity hitEntity = ecs::InvalidEntity;
        ctx.registry->view<BodyLink>([&](ecs::Entity e, BodyLink& b)
        {
            if (b.name == hit.name) hitEntity = e;
        });

        if (hitEntity == ecs::InvalidEntity)
        {
            // Environment hit: nothing to damage
            return;
        }

        Health* hp = ctx.registry->find<Health>(hitEntity);
        if (hp != NULL && hp->hp > 0.0f)
        {
            hp->hp -= 1.0f;
            if (hp->hp <= 0.0f)
            {
                TargetTag* t = ctx.registry->find<TargetTag>(hitEntity);
                if (t != NULL)
                {
                    t->dead = true; t->respawnTimer = t->respawnDelay;
                    NodeLink* nl = ctx.registry->find<NodeLink>(hitEntity);
                    if (nl != NULL && nl->node.valid()) nl->node->setNodeMask(0);
                    ctx.physics->setCollisionEnabled(hit.name, false);
                }
                ctx.score += 10;
                ctx.hud->setScore(ctx.score);
                ctx.hud->showMessage("Target destroyed!", 1.2f);
            }
        }
        else
        {
            // Dynamic prop: knock it back along the shot direction
            ctx.physics->applyImpulse(hit.name, hit.position, dir * 4.0f);
        }
    }

    void updateTargets(GameContext& ctx, double dt)
    {
        ctx.registry->view<TargetTag>([&](ecs::Entity e, TargetTag& t)
        {
            if (!t.dead) return;
            t.respawnTimer -= (float)dt;
            if (t.respawnTimer > 0.0f) return;

            t.dead = false;
            Health* hp = ctx.registry->find<Health>(e);
            if (hp != NULL) hp->hp = hp->maxHp;
            NodeLink* nl = ctx.registry->find<NodeLink>(e);
            if (nl != NULL && nl->node.valid()) nl->node->setNodeMask(~0u);
            BodyLink* bl = ctx.registry->find<BodyLink>(e);
            if (bl != NULL)
            {
                ctx.physics->setTransform(bl->name, t.spawnPose);
                ctx.physics->setCollisionEnabled(bl->name, true);
            }
        });
    }

    void updateEffects(GameContext& ctx, double dt)
    {
        ctx.registry->view<Lifetime>([&](ecs::Entity e, Lifetime& l)
        {
            l.remaining -= (float)dt;
            if (l.remaining > 0.0f) return;
            NodeLink* nl = ctx.registry->find<NodeLink>(e);
            if (nl != NULL && nl->node.valid() && nl->node->getNumParents() > 0)
                nl->node->getParent(0)->removeChild(nl->node.get());
            ctx.registry->destroy(e);
        });
        ctx.registry->flush();
    }

    void updateHud(GameContext& ctx, double dt)
    {
        ctx.hud->update(dt);
    }
}
