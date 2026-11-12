#ifndef MANA_MODELING_MESH_FIXER_HPP
#define MANA_MODELING_MESH_FIXER_HPP

#include <osg/Geometry>
#include <vector>
#include <utility>

namespace osgVerse
{
    /** Topology inspection and repair helpers, implemented with cinolib.
        When osgVerse is built without cinolib (VERSE_WITH_CINOLIB undefined), every
        method reports a "not implemented" notice and returns a failure value.
        All methods take/return plain osg::Geometry with triangle data, so cinolib
        types never leak into the public interface.
    */
    class MeshFixer
    {
    public:
        /** Return true if cinolib support is compiled in */
        static bool isSupported();

        /** Find all pairs of self-intersecting triangles. Each pair contains triangle
            indices which follow the triangle-soup order of the input geometry.
            Return the number of detected pairs. */
        static unsigned int findSelfIntersections(
            osg::Geometry& geom, std::vector<std::pair<unsigned int, unsigned int>>& pairs);

        /** Split non-manifold vertices and edges into 2-manifold fans. Faces around a
            non-manifold edge are ordered geometrically (cinolib predicates) before they
            are assigned to different fans, so every vertex copy is a disk-like fan.
            Return a new geometry with duplicated vertices, or NULL on failure. The
            output only keeps positions and normals (UV/color arrays are dropped). */
        static osg::Geometry* fixNonManifoldEdges(osg::Geometry& geom);

        /** Split the input geometry into its connected components, one geometry each */
        static std::vector<osg::ref_ptr<osg::Geometry>> splitConnectedComponents(osg::Geometry& geom);

        /** Options of the solidification (voxel based watertight remeshing) */
        struct SolidifyOptions
        {
            SolidifyOptions()
            : resolution(64), smooth(true), smoothIterations(3) {}

            int resolution;         // maximum number of voxels per side
            bool smooth;            // smooth the result and reproject it to the input surface
            int smoothIterations;   // number of smoothing iterations
        };

        /** Statistics and volume quality of the solidification */
        struct SolidifyInfo
        {
            SolidifyInfo()
            : numTetrahedra(0), numVertices(0), numTriangles(0),
              minScaledJacobian(0.0), avgScaledJacobian(0.0)
            { gridSize[0] = gridSize[1] = gridSize[2] = 0; }

            int gridSize[3];        // resolution of the voxel grid
            int numTetrahedra;      // number of tetrahedra used for marching tets
            int numVertices;        // number of vertices of the output surface
            int numTriangles;       // number of triangles of the output surface
            double minScaledJacobian, avgScaledJacobian;  // quality of the tetrahedra
        };

        /** Convert an arbitrary triangle mesh (open / non-manifold / self-intersecting)
            into a single watertight 2-manifold surface: solid voxelization, then
            hexahedra->tetrahedra conversion, then marching tets. Optionally the result
            is smoothed and reprojected onto the input surface.
            Return NULL on failure. The output only keeps positions and normals. */
        static osg::Geometry* solidify(osg::Geometry& geom,
                                       const SolidifyOptions& opt = SolidifyOptions(),
                                       SolidifyInfo* info = NULL);
    };
}

#endif
