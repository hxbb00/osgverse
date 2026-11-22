#ifndef VERSE_STRIKE_EFFECTS_H
#define VERSE_STRIKE_EFFECTS_H

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/Math>
#include <animation/ParticleEngine.h>
#include <pipeline/Pipeline.h>
#include "Ecs.h"
#include "Components.h"

namespace game
{
    /** Transient visual effects: bullet tracers (short-lived stretched boxes, ECS-managed)
        and an optional Effekseer flash (muzzle/impact). When Effekseer is not compiled in
        or no effect file is present, everything still works and just looks simpler. */
    class Effects
    {
    public:
        Effects(ecs::Registry* registry, osg::Group* fxRoot)
          : _registry(registry), _root(fxRoot), _flashLoaded(false)
        {
#ifdef VERSE_STRIKE_WITH_EFFEKSEER
            _particles = new osgVerse::ParticleDrawableEffekseer;
            _particles->createEffect("flash", std::string(BASE_DIR) + "/models/Particles/Fireworks.efkefc");
            osg::StateSet* ss = _particles->getOrCreateStateSet();
            ss->setMode(GL_BLEND, osg::StateAttribute::ON);
            ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

            _flashMT = new osg::MatrixTransform;
            _flashMT->addChild(_particles.get());
            osgVerse::Pipeline::setPipelineMask(*_flashMT, FORWARD_SCENE_MASK);
            _flashMT->setNodeMask(0);  // hidden until played
            _root->addChild(_flashMT.get());
            _flashLoaded = _particles->getEffectState("flash")
                         != osgVerse::ParticleDrawableEffekseer::INVALID;
#endif
        }

        void tracer(const osg::Vec3& from, const osg::Vec3& to, float life = 0.06f)
        {
            osg::Vec3 dir = to - from;
            float len = dir.length();
            if (len < 0.01f) return;
            dir /= len;

            osg::Matrix rotM = osg::Matrix::rotate(osg::Vec3(0.0f, 0.0f, 1.0f), dir);
            osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(
                new osg::Box(osg::Vec3(), 0.02f, 0.02f, 1.0f));
            sd->setColor(osg::Vec4(1.0f, 0.9f, 0.5f, 1.0f));
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(sd.get());

            osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
            mt->setMatrix(osg::Matrix::translate(from) * rotM
                        * osg::Matrix::scale(1.0f, 1.0f, len) * osg::Matrix::translate(0.0f, 0.0f, 0.5f));
            mt->addChild(geode.get());
            _root->addChild(mt.get());

            ecs::Entity e = _registry->create();
            _registry->emplace<NodeLink>(e, mt.get());
            _registry->emplace<Lifetime>(e, life);
        }

        void flash(const osg::Vec3& pos, float scale = 0.15f)
        {
#ifdef VERSE_STRIKE_WITH_EFFEKSEER
            if (!_flashLoaded) return;
            _flashMT->setMatrix(osg::Matrix::scale(scale, scale, scale) * osg::Matrix::translate(pos));
            _flashMT->setNodeMask(~0u);
            _particles->playEffect("flash", osgVerse::ParticleDrawableEffekseer::PLAYING);
#endif
        }

    protected:
        ecs::Registry* _registry;
        osg::Group* _root;
        osg::ref_ptr<osgVerse::ParticleDrawableEffekseer> _particles;
        osg::ref_ptr<osg::MatrixTransform> _flashMT;
        bool _flashLoaded;
    };
}

#endif
