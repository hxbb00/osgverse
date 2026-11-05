#ifndef MANA_READERWRITER_MESHINTERNALOPTIMIZER_HPP
#define MANA_READERWRITER_MESHINTERNALOPTIMIZER_HPP

#include <osg/Referenced>
#include <osg/Geometry>
#include <map>
#include <set>
#include "Export.h"

namespace osgVerse
{
    /** Remove mesh triangles which can never be seen, together with vertices that become
        isolated after the removal; all other vertex attributes and state sets are kept.
        The model is rendered as its own occluder by the software rasterizer (see
        osgVerse::OcclusionAnalyzer) from camera views orbiting around its bounding sphere, and
        only triangles proven invisible from every sampled view are removed, so the result is
        conservative: no visible triangle is ever deleted. The analysis is repeated until no
        more triangles can be removed, as vertices (and therefore quad groupings) change.
        This is useful for building/BIM models which contain complete interior structures,
        floors and inner walls that are always hidden by the outer shell.
        Note: a smaller angle step gives a stricter result but costs more time; transparent
        materials are treated as opaque, so surfaces behind glass may be removed as well. */
    class OSGVERSE_RW_EXPORT MeshInternalOptimizer : public osg::Referenced
    {
    public:
        MeshInternalOptimizer(float angleStep = 30.0f, float horizontalStart = 0.0f,
                              float verticalStart = -90.0f, float verticalRange = 180.0f);
        virtual ~MeshInternalOptimizer();

        /** Angular step in degrees used by both horizontal and vertical orbiting */
        void setAngleStep(float deg) { _angleStep = deg; }
        float getAngleStep() const { return _angleStep; }

        /** Horizontal (azimuth) orbiting start angle and range in degrees */
        void setHorizontalRange(float startDeg, float rangeDeg = 360.0f)
        { _horizontalStart = startDeg; _horizontalRange = rangeDeg; }
        float getHorizontalStart() const { return _horizontalStart; }
        float getHorizontalRange() const { return _horizontalRange; }

        /** Vertical (elevation) orbiting start angle and range in degrees. The default covers
            the whole sphere (-90 to +90); use e.g. (0, 90) to ignore the bottom of a model
            standing on the ground, which may be open but its inner faces are still invisible */
        void setVerticalRange(float startDeg, float rangeDeg = 180.0f)
        { _verticalStart = startDeg; _verticalRange = rangeDeg; }
        float getVerticalStart() const { return _verticalStart; }
        float getVerticalRange() const { return _verticalRange; }

        /** Rasterization resolution used by each camera view (multiples of 8) */
        void setResolution(unsigned int w, unsigned int h) { _width = w; _height = h; }
        unsigned int getResolutionWidth() const { return _width; }
        unsigned int getResolutionHeight() const { return _height; }

        void setCameraFov(float deg) { _fov = deg; }
        void setOrbitDistanceScale(float s) { _distanceScale = s; }

        /** Optimize the given model in place (modifying its geometries) and return it */
        osg::Node* optimize(osg::Node* node);

        unsigned int getNumRemovedTriangles() const { return _removedTriangles; }
        unsigned int getNumRemovedVertices() const { return _removedVertices; }
        unsigned int getNumViews() const { return _numViews; }

    protected:
        void removeTriangles(osg::Geometry& geom, const std::map<unsigned int, std::set<unsigned int>>& removals);

        float _angleStep, _horizontalStart, _horizontalRange, _verticalStart, _verticalRange;
        float _fov, _distanceScale;
        unsigned int _width, _height;
        unsigned int _removedTriangles, _removedVertices, _numViews;
    };
}

#endif
