#include <osg/Notify>
#include "Predicates.h"

#ifdef VERSE_WITH_CINOLIB
#   include <cinolib/predicates.h>
#endif

namespace
{
#ifdef VERSE_WITH_CINOLIB
    inline cinolib::vec2d toCino(const osg::Vec2d& v) { return cinolib::vec2d(v[0], v[1]); }
    inline cinolib::vec3d toCino(const osg::Vec3d& v) { return cinolib::vec3d(v[0], v[1], v[2]); }
#endif

    inline void reportNotImplemented(const char* func)
    {
        OSG_NOTICE << "[osgVerse] " << func << "(): not implemented, cinolib is not found"
                   << std::endl;
    }
}

namespace osgVerse
{
    bool isPredicateSupported()
    {
#ifdef VERSE_WITH_CINOLIB
        return true;
#else
        return false;
#endif
    }

    int predicateSign(double value)
    {
        return (value > 0.0) ? 1 : ((value < 0.0) ? -1 : 0);
    }

    double orient2D(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::orient2d(toCino(a), toCino(b), toCino(c));
#else
        reportNotImplemented("orient2D");
        return 0.0;
#endif
    }

    double orient3D(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                    const osg::Vec3d& d)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::orient3d(toCino(a), toCino(b), toCino(c), toCino(d));
#else
        reportNotImplemented("orient3D");
        return 0.0;
#endif
    }

    double inCircle(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c,
                    const osg::Vec2d& d)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::incircle(toCino(a), toCino(b), toCino(c), toCino(d));
#else
        reportNotImplemented("inCircle");
        return 0.0;
#endif
    }

    double inSphere(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                    const osg::Vec3d& d, const osg::Vec3d& e)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::insphere(toCino(a), toCino(b), toCino(c), toCino(d), toCino(e));
#else
        reportNotImplemented("inSphere");
        return 0.0;
#endif
    }

    bool arePointsColinear(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::points_are_colinear_2d(toCino(a), toCino(b), toCino(c));
#else
        reportNotImplemented("arePointsColinear");
        return false;
#endif
    }

    bool arePointsColinear(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::points_are_colinear_3d(toCino(a), toCino(b), toCino(c));
#else
        reportNotImplemented("arePointsColinear");
        return false;
#endif
    }

    bool arePointsCoplanar(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                           const osg::Vec3d& d)
    {
#ifdef VERSE_WITH_CINOLIB
        return cinolib::points_are_coplanar_3d(toCino(a), toCino(b), toCino(c), toCino(d));
#else
        reportNotImplemented("arePointsCoplanar");
        return false;
#endif
    }
}
