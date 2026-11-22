#ifndef VERSE_STRIKE_HUD_H
#define VERSE_STRIKE_HUD_H

#include <osg/Camera>
#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osgText/Text>
#include <pipeline/Pipeline.h>
#include <pipeline/Global.h>

namespace game
{
    /** Minimal HUD: crosshair + score + control hints, drawn on an ortho overlay camera */
    class Hud
    {
    public:
        Hud(osg::Group* root, int renderOrder = 10001)
          : _scoreText(NULL), _messageText(NULL), _messageTime(0.0f)
        {
            _camera = new osg::Camera;
            _camera->setClearMask(GL_DEPTH_BUFFER_BIT);
            _camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            _camera->setProjectionMatrix(osg::Matrix::ortho2D(0.0, 1.0, 0.0, 1.0));
            _camera->setViewMatrix(osg::Matrix::identity());
            _camera->setRenderOrder(osg::Camera::POST_RENDER, renderOrder);
            _camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            _camera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
            osgVerse::Pipeline::setPipelineMask(*_camera, FORWARD_SCENE_MASK);
            root->addChild(_camera.get());

            // Crosshair: two thin quads centered on the screen
            const osg::Vec4 crossColor(0.9f, 0.9f, 0.9f, 1.0f);
            addQuad(osg::Vec3(0.5f, 0.5f, -0.1f), 0.014f, 0.002f, crossColor);
            addQuad(osg::Vec3(0.5f, 0.5f, -0.1f), 0.002f, 0.014f, crossColor);

            _scoreText = addText(osg::Vec3(0.02f, 0.95f, 0.0f), "Score: 0", 0.035f);
            addText(osg::Vec3(0.02f, 0.06f, 0.0f),
                    "WASD move / Space jump / Shift sprint / LMB or F shoot / ESC quit", 0.02f);
            _messageText = addText(osg::Vec3(0.5f, 0.72f, 0.0f), "", 0.03f);
            _messageText->setAlignment(osgText::Text::CENTER_BOTTOM);
        }

        void setScore(int s) { _scoreText->setText("Score: " + std::to_string(s)); }

        void showMessage(const std::string& m, float time = 1.5f)
        {
            _messageText->setText(m); _messageTime = time;
        }

        void update(double dt)
        {
            if (_messageTime > 0.0f)
            {
                _messageTime -= dt;
                if (_messageTime <= 0.0f) _messageText->setText("");
            }
        }

    protected:
        void addQuad(const osg::Vec3& center, float w, float h, const osg::Vec4& color)
        {
            osg::ref_ptr<osg::ShapeDrawable> sd = new osg::ShapeDrawable(
                new osg::Box(center, w, h, 0.5f));
            sd->setColor(color);
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(sd.get());
            _camera->addChild(geode.get());
        }

        osgText::Text* addText(const osg::Vec3& pos, const std::string& text, float size)
        {
            osg::ref_ptr<osgText::Text> t = new osgText::Text;
            t->setDataVariance(osg::Object::DYNAMIC);
            t->setPosition(pos);
            t->setAlignment(osgText::Text::LEFT_BOTTOM);
            t->setCharacterSize(size);
            t->setCharacterSizeMode(osgText::Text::OBJECT_COORDS);
            t->setColor(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
            t->setFont(MISC_DIR + "LXGWFasmartGothic.ttf");
            t->setText(text);
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(t.get());
            _camera->addChild(geode.get());
            return t.release();
        }

        osg::ref_ptr<osg::Camera> _camera;
        osgText::Text* _scoreText;
        osgText::Text* _messageText;
        float _messageTime;
    };
}

#endif
