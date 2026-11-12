#ifndef MANA_MODELING_PREDICATES_HPP
#define MANA_MODELING_PREDICATES_HPP

#include <osg/Vec2d>
#include <osg/Vec3d>

namespace osgVerse
{
    /** Robust geometric predicates, wrapped from cinolib's adaptive predicates.
        Results are exact only when cinolib is compiled with the symbol
        CINOLIB_USES_SHEWCHUK_PREDICATES; otherwise they are the "fast" floating point
        approximations (same as Shewchuk's fast path).
        When osgVerse is built without cinolib, every function reports
        "not implemented" and returns a neutral value (0 or false).
    */
    extern bool isPredicateSupported();

    /** Return -1, 0 or +1 according to the sign of a predicate result */
    extern int predicateSign(double value);

    /** Twice the signed area of triangle (a,b,c); positive if counter-clockwise */
    extern double orient2D(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c);

    /** Signed volume of tetrahedron (a,b,c,d); positive if d is above (a,b,c) */
    extern double orient3D(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                           const osg::Vec3d& d);

    /** Positive if d is inside the circle through (a,b,c), negative if outside */
    extern double inCircle(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c,
                           const osg::Vec2d& d);

    /** Positive if e is inside the sphere through (a,b,c,d), negative if outside */
    extern double inSphere(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                           const osg::Vec3d& d, const osg::Vec3d& e);

    /** Check if three points are collinear */
    extern bool arePointsColinear(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c);
    extern bool arePointsColinear(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c);

    /** Check if four points are coplanar */
    extern bool arePointsCoplanar(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c,
                                  const osg::Vec3d& d);
}

#endif
