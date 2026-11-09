#ifndef MANA_ANIM_UTILITIES_HPP
#define MANA_ANIM_UTILITIES_HPP

#include <osg/Version>
#include <osg/Texture2D>
#include <osg/Shape>
#include <osg/Geometry>
#include <osg/Callback>
#include <thread>
#include <functional>
#include "PhysicsEngine.h"

namespace osgVerse
{

    /** Update physics pose callback */
    class PhysicsUpdateCallback : public osg::NodeCallback
    {
    public:
        PhysicsUpdateCallback() {}
        PhysicsUpdateCallback(PhysicsEngine* e, const std::string& n);
        PhysicsUpdateCallback(const PhysicsUpdateCallback& copy,
                              const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
        :   osg::NodeCallback(copy, copyop), _engine(copy._engine), _bodyName(copy._bodyName) {}
        META_Object(osgVerse, PhysicsUpdateCallback);
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv);

        void setEngine(PhysicsEngine* e) { _engine = e; }
        const PhysicsEngine* getEngine() const { return _engine.get(); }
        void setBodyName(const std::string& n) { _bodyName = n; }
        const std::string& getBodyName() const { return _bodyName; }

    protected:
        osg::ref_ptr<PhysicsEngine> _engine;  // also keeps a deserialized engine alive
        std::string _bodyName;
    };

    /** Vector smoothing filter base */
    struct FilterBase : public osg::Referenced
    { virtual double filter(double input) = 0; };

    /** Vector smoother */
    class VectorSmoother : public osg::Referenced
    {
    public:
        VectorSmoother(double sampleRate = 100);

        osg::Vec3 filter(const osg::Vec3& in) const
        {
            return osg::Vec3(_filter[0]->filter(in[0]), _filter[1]->filter(in[1]),
                             _filter[2]->filter(in[2]));
        }

    protected:
        osg::ref_ptr<FilterBase> _filter[3];
    };

    /** Time-out trigger */
    class TimeOut : public osg::Referenced
    {
    public:
        TimeOut();
        void* getTimeWheel() { return _timeWheel; }
        std::chrono::steady_clock::time_point getStart() { return _start; }
        bool running() const { return _running; }

        typedef void TimeOutCallback(osg::Referenced*);
        void set(osg::Referenced* sender, TimeOutCallback cb, double seconds, bool repeated);
        void set(osg::Referenced* sender, TimeOutCallback cb,
                 std::chrono::steady_clock::time_point time);
        bool checkTimeouts();

    protected:
        virtual ~TimeOut();

        void* _timeWheel;
        std::chrono::steady_clock::time_point _start;
        std::thread _thread;
        bool _running;
    };

}

#endif
