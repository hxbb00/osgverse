#ifndef VERSE_STRIKE_FPSCONTROLLER_H
#define VERSE_STRIKE_FPSCONTROLLER_H

#include <osgViewer/Viewer>
#include <osgGA/GUIEventHandler>
#include <osg/Math>

namespace game
{
    /** First-person input handler: WASD move + mouse look (pointer re-centering),
        Space = jump, Shift = sprint, LMB = fire, ESC = quit. The camera matrix is
        updated manually each frame via applyCamera() from the character controller. */
    class FpsController : public osgGA::GUIEventHandler
    {
    public:
        FpsController()
          : _yaw(osg::PI_2), _pitch(0.0f), _sensitivity(0.0022f), _captured(false),
            _sprint(false), _fireRequest(false), _jumpRequest(false)
        { for (int i = 0; i < 4; ++i) _keys[i] = false; }

        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
        {
            osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
            switch (ea.getEventType())
            {
            case osgGA::GUIEventAdapter::PUSH:
                if (ea.getButtonMask() & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
                    _fireRequest = true;
                setCaptured(view, true);
                return true;
            case osgGA::GUIEventAdapter::MOVE:
                if (_captured) return updateMouse(view, ea);
                break;
            case osgGA::GUIEventAdapter::KEYDOWN:
                switch (ea.getKey())
                {
                case osgGA::GUIEventAdapter::KEY_W: _keys[0] = true; break;
                case osgGA::GUIEventAdapter::KEY_S: _keys[1] = true; break;
                case osgGA::GUIEventAdapter::KEY_A: _keys[2] = true; break;
                case osgGA::GUIEventAdapter::KEY_D: _keys[3] = true; break;
                case osgGA::GUIEventAdapter::KEY_Space: _jumpRequest = true; break;
                case osgGA::GUIEventAdapter::KEY_Shift_L: _sprint = true; break;
                case 'f': case 'F': _fireRequest = true; break;  // keyboard alias for firing
                case osgGA::GUIEventAdapter::KEY_Escape:
                    {
                        osgViewer::Viewer* v = dynamic_cast<osgViewer::Viewer*>(&aa);
                        if (v) v->setDone(true);
                    }
                    break;
                default: break;
                }
                break;
            case osgGA::GUIEventAdapter::KEYUP:
                switch (ea.getKey())
                {
                case osgGA::GUIEventAdapter::KEY_W: _keys[0] = false; break;
                case osgGA::GUIEventAdapter::KEY_S: _keys[1] = false; break;
                case osgGA::GUIEventAdapter::KEY_A: _keys[2] = false; break;
                case osgGA::GUIEventAdapter::KEY_D: _keys[3] = false; break;
                case osgGA::GUIEventAdapter::KEY_Shift_L: _sprint = false; break;
                default: break;
                }
                break;
            default: break;
            }
            return false;
        }

        /** Horizontal wish direction (length <= 1) computed from the camera yaw */
        osg::Vec3 wishDirection() const
        {
            osg::Vec3 forward(cosf(_yaw), sinf(_yaw), 0.0f);
            osg::Vec3 right(forward.y(), -forward.x(), 0.0f);
            osg::Vec3 wish = forward * (_keys[0] ? 1.0f : (_keys[1] ? -1.0f : 0.0f))
                           + right * (_keys[3] ? 1.0f : (_keys[2] ? -1.0f : 0.0f));
            if (wish.length() > 1.0f) wish.normalize();
            return wish;
        }

        /** Full 3D view direction including pitch */
        osg::Vec3 viewDirection() const
        {
            float cp = cosf(_pitch);
            return osg::Vec3(cosf(_yaw) * cp, sinf(_yaw) * cp, sinf(_pitch));
        }

        bool consumeFire() { bool b = _fireRequest; _fireRequest = false; return b; }
        bool consumeJump() { bool b = _jumpRequest; _jumpRequest = false; return b; }
        bool isSprinting() const { return _sprint; }
        bool isCaptured() const { return _captured; }

        void applyCamera(osg::Camera* cam, const osg::Vec3& eye)
        {
            cam->setViewMatrixAsLookAt(eye, eye + viewDirection(), osg::Z_AXIS);
        }

    protected:
        void setCaptured(osgViewer::View* view, bool b)
        {
            if (_captured == b) return;
            _captured = b;
            osgViewer::GraphicsWindow* gw = getWindow(view);
            if (!gw) return;
            gw->setCursor(b ? osgViewer::GraphicsWindow::NoCursor
                            : osgViewer::GraphicsWindow::InheritCursor);
            if (b)
                gw->requestWarpPointer(0.5f * gw->getTraits()->width, 0.5f * gw->getTraits()->height);
        }

        bool updateMouse(osgViewer::View* view, const osgGA::GUIEventAdapter& ea)
        {
            osgViewer::GraphicsWindow* gw = getWindow(view);
            if (!gw) return false;
            float cx = 0.5f * gw->getTraits()->width, cy = 0.5f * gw->getTraits()->height;
            _yaw -= (ea.getX() - cx) * _sensitivity;
            _pitch -= (ea.getY() - cy) * _sensitivity;
            if (_pitch > 1.5f) _pitch = 1.5f;
            else if (_pitch < -1.5f) _pitch = -1.5f;
            gw->requestWarpPointer(cx, cy);
            return true;
        }

        static osgViewer::GraphicsWindow* getWindow(osgViewer::View* view)
        {
            if (!view || !view->getCamera()) return NULL;
            return dynamic_cast<osgViewer::GraphicsWindow*>(view->getCamera()->getGraphicsContext());
        }

        float _yaw, _pitch, _sensitivity;
        bool _captured, _sprint, _fireRequest, _jumpRequest, _keys[4];
    };
}

#endif
