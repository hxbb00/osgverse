#include <osg/Notify>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "MeshFixer.h"
#include "Utilities.h"

#ifdef VERSE_WITH_CINOLIB
#   include <cinolib/meshes/trimesh.h>
#   include <cinolib/predicates.h>
#   include <cinolib/find_intersections.h>
#   include <cinolib/non_manifold_edge_geometric_sorting.h>
#   include <cinolib/connected_components.h>
#   include <cinolib/voxelize.h>
#   include <cinolib/serialize_index.h>
#   include <cinolib/tetrahedralization.h>
#   include <cinolib/isosurface.h>
#   include <cinolib/quality_tet.h>
#   include <cinolib/vector_serialization.h>
#   include <cinolib/octree.h>
#endif

using namespace osgVerse;

namespace
{
    bool collectTriangles(osg::Geometry& geom, std::vector<osg::Vec3>& verts,
                          std::vector<unsigned int>& tris)
    {
        MeshCollector collector;
        collector.setWeldingVertices(true);
        collector.setOnlyVertexAndIndices(true);
        collector.apply(geom);
        verts = collector.getVertices();
        tris = collector.getTriangles();
        return (verts.size() > 3 && tris.size() > 2);
    }

    void reportNotImplemented(const char* func)
    {
        OSG_NOTICE << "[osgVerse] MeshFixer::" << func
                   << "(): not implemented, cinolib is not found" << std::endl;
    }

#ifdef VERSE_WITH_CINOLIB
    struct DisjointSet
    {
        explicit DisjointSet(size_t n) : _parent(n)
        {
            for (size_t i = 0; i < n; ++i) _parent[i] = i;
        }

        size_t find(size_t i)
        {
            while (_parent[i] != i) { _parent[i] = _parent[_parent[i]]; i = _parent[i]; }
            return i;
        }

        void unite(size_t a, size_t b)
        {
            a = find(a); b = find(b);
            if (a != b) _parent[b] = a;
        }

        std::vector<size_t> _parent;
    };

    osg::Geometry* createTriangleGeometry(const std::vector<osg::Vec3>& verts,
                                          const std::vector<unsigned int>& indices)
    {
        if (verts.empty() || indices.size() < 3) return NULL;

        osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) (*va)[i] = verts[i];

        osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
        de->assign(indices.begin(), indices.end());
        return createGeometry(va.get(), NULL, NULL, de.get());
    }

    inline cinolib::vec3d toCino(const osg::Vec3& v)
    {
        return cinolib::vec3d(v[0], v[1], v[2]);
    }

    inline void toCinolib(const std::vector<osg::Vec3>& verts, std::vector<double>& coords)
    {
        coords.resize(verts.size() * 3);
        for (size_t i = 0; i < verts.size(); ++i)
        {
            const osg::Vec3& v = verts[i];
            coords[i * 3 + 0] = v[0]; coords[i * 3 + 1] = v[1]; coords[i * 3 + 2] = v[2];
        }
    }
#endif
}

bool MeshFixer::isSupported()
{
#ifdef VERSE_WITH_CINOLIB
    return true;
#else
    return false;
#endif
}

unsigned int MeshFixer::findSelfIntersections(
    osg::Geometry& geom, std::vector<std::pair<unsigned int, unsigned int>>& pairs)
{
    pairs.clear();
#ifdef VERSE_WITH_CINOLIB
    std::vector<osg::Vec3> verts; std::vector<unsigned int> tris;
    if (!collectTriangles(geom, verts, tris)) return 0;

    std::vector<cinolib::vec3d> coords(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) coords[i] = toCino(verts[i]);

    std::set<cinolib::ipair> intersections;
    cinolib::find_intersections(coords, tris, intersections);
    pairs.reserve(intersections.size());
    for (const cinolib::ipair& p : intersections)
        pairs.push_back(std::make_pair((unsigned int)p.first, (unsigned int)p.second));
    return (unsigned int)pairs.size();
#else
    reportNotImplemented("findSelfIntersections");
    return 0U;
#endif
}

osg::Geometry* MeshFixer::fixNonManifoldEdges(osg::Geometry& geom)
{
#ifdef VERSE_WITH_CINOLIB
    std::vector<osg::Vec3> verts; std::vector<unsigned int> tris;
    if (!collectTriangles(geom, verts, tris)) return NULL;

    const unsigned int numVerts = (unsigned int)verts.size();
    const unsigned int numTris = (unsigned int)(tris.size() / 3);
    typedef std::pair<unsigned int, unsigned int> EdgeKey;

    // collect faces of each edge and each vertex
    std::map<EdgeKey, std::vector<unsigned int>> edgeFaces;
    std::vector<std::vector<unsigned int>> vertFaces(numVerts);
    for (unsigned int t = 0; t < numTris; ++t)
    {
        for (int c = 0; c < 3; ++c)
        {
            unsigned int v0 = tris[t * 3 + c], v1 = tris[t * 3 + (c + 1) % 3];
            edgeFaces[EdgeKey(std::min(v0, v1), std::max(v0, v1))].push_back(t);
        }
        for (int c = 0; c < 3; ++c) vertFaces[tris[t * 3 + c]].push_back(t);
    }

    // split every vertex into disk-like fans; a face pair is connected if they share an
    // ordinary edge, or if they are consecutive in the geometric ordering around a
    // non-manifold edge
    std::vector<int> duplicated(tris.size(), -1);
    std::vector<osg::Vec3> newVerts(verts);
    for (unsigned int v = 0; v < numVerts; ++v)
    {
        const std::vector<unsigned int>& faces = vertFaces[v];
        if (faces.size() < 2) continue;

        std::map<unsigned int, unsigned int> faceToLocal;
        for (unsigned int i = 0; i < (unsigned int)faces.size(); ++i)
            faceToLocal[faces[i]] = i;

        DisjointSet dsu(faces.size());
        std::set<EdgeKey> processed;
        for (unsigned int t : faces)
        {
            for (int c = 0; c < 3; ++c)
            {
                unsigned int v0 = tris[t * 3 + c], v1 = tris[t * 3 + (c + 1) % 3];
                if (v0 != v && v1 != v) continue;

                unsigned int x = (v0 == v) ? v1 : v0;
                EdgeKey key(std::min(v, x), std::max(v, x));
                if (!processed.insert(key).second) continue;

                const std::vector<unsigned int>& ef = edgeFaces[key];
                if (ef.size() == 2)
                    dsu.unite(faceToLocal[ef[0]], faceToLocal[ef[1]]);
                else if (ef.size() > 2)
                {
                    std::vector<cinolib::vec3d> opposite;
                    opposite.reserve(ef.size());
                    for (unsigned int ft : ef)
                    {
                        for (int k = 0; k < 3; ++k)
                        {
                            unsigned int id = tris[ft * 3 + k];
                            if (id != v && id != x) { opposite.push_back(toCino(verts[id])); break; }
                        }
                    }

                    std::vector<unsigned int> ordering;
                    // the edge is always given in the same direction, so that both endpoints
                    // produce the same circular order and pair the same faces together
                    cinolib::non_manifold_edge_geometric_sorting(
                        toCino(verts[std::min(v, x)]), toCino(verts[std::max(v, x)]),
                        opposite, ordering);
                    for (size_t i = 0; i + 1 < ordering.size(); i += 2)
                        dsu.unite(faceToLocal[ef[ordering[i]]], faceToLocal[ef[ordering[i + 1]]]);
                }
            }
        }

        std::map<unsigned int, std::vector<unsigned int>> fans;
        for (unsigned int i = 0; i < (unsigned int)faces.size(); ++i)
            fans[(unsigned int)dsu.find(i)].push_back(i);
        if (fans.size() < 2) continue;

        bool firstFan = true;
        for (const auto& fan : fans)
        {
            if (firstFan) { firstFan = false; continue; }  // the first fan keeps the vertex
            unsigned int newId = (unsigned int)newVerts.size();
            newVerts.push_back(verts[v]);
            for (unsigned int localIndex : fan.second)
            {
                unsigned int t = faces[localIndex];
                for (int k = 0; k < 3; ++k)
                {
                    if (tris[t * 3 + k] == v) { duplicated[t * 3 + k] = (int)newId; break; }
                }
            }
        }
    }

    bool changed = false;
    for (size_t i = 0; i < duplicated.size(); ++i)
    {
        if (duplicated[i] >= 0) { changed = true; break; }
    }
    if (!changed)
    {
        OSG_INFO << "[osgVerse] MeshFixer::fixNonManifoldEdges(): nothing to fix" << std::endl;
        return NULL;
    }

    std::vector<unsigned int> newIndices(tris);
    for (size_t i = 0; i < newIndices.size(); ++i)
    {
        if (duplicated[i] >= 0) newIndices[i] = (unsigned int)duplicated[i];
    }
    return createTriangleGeometry(newVerts, newIndices);
#else
    reportNotImplemented("fixNonManifoldEdges");
    return NULL;
#endif
}

std::vector<osg::ref_ptr<osg::Geometry>> MeshFixer::splitConnectedComponents(osg::Geometry& geom)
{
    std::vector<osg::ref_ptr<osg::Geometry>> result;
#ifdef VERSE_WITH_CINOLIB
    std::vector<osg::Vec3> verts; std::vector<unsigned int> tris;
    if (!collectTriangles(geom, verts, tris)) return result;

    std::vector<double> coords;
    toCinolib(verts, coords);
    cinolib::Trimesh<> mesh(coords, tris);

    std::vector<std::unordered_set<unsigned int>> components;
    cinolib::connected_components(mesh, components);

    std::vector<int> vertComponent(mesh.num_verts(), -1);
    for (unsigned int i = 0; i < (unsigned int)components.size(); ++i)
        for (unsigned int vid : components[i]) vertComponent[vid] = (int)i;

    std::vector<std::vector<unsigned int>> componentFaces(components.size());
    for (unsigned int pid = 0; pid < mesh.num_polys(); ++pid)
    {
        int c = vertComponent[mesh.poly_vert_id(pid, 0)];
        if (c >= 0) componentFaces[c].push_back(pid);
    }

    for (const auto& faces : componentFaces)
    {
        if (faces.empty()) continue;

        std::vector<osg::Vec3> newVerts; std::vector<unsigned int> newIndices;
        std::unordered_map<unsigned int, unsigned int> localMap;
        for (unsigned int pid : faces)
        {
            for (unsigned int k = 0; k < 3; ++k)
            {
                unsigned int vid = mesh.poly_vert_id(pid, k);
                std::unordered_map<unsigned int, unsigned int>::iterator it = localMap.find(vid);
                if (it == localMap.end())
                {
                    unsigned int local = (unsigned int)newVerts.size();
                    it = localMap.insert(std::make_pair(vid, local)).first;
                    newVerts.push_back(osg::Vec3(mesh.vert(vid)[0], mesh.vert(vid)[1],
                                                 mesh.vert(vid)[2]));
                }
                newIndices.push_back(it->second);
            }
        }

        osg::ref_ptr<osg::Geometry> newGeom = createTriangleGeometry(newVerts, newIndices);
        if (newGeom.valid()) result.push_back(newGeom);
    }
#else
    reportNotImplemented("splitConnectedComponents");
#endif
    return result;
}

osg::Geometry* MeshFixer::solidify(osg::Geometry& geom, const SolidifyOptions& opt,
                                   SolidifyInfo* info)
{
#ifdef VERSE_WITH_CINOLIB
    std::vector<osg::Vec3> verts; std::vector<unsigned int> tris;
    if (!collectTriangles(geom, verts, tris))
    {
        OSG_WARN << "[osgVerse] MeshFixer::solidify(): invalid input geometry" << std::endl;
        return NULL;
    }

    std::vector<double> coords;
    toCinolib(verts, coords);
    cinolib::Trimesh<> inMesh(coords, tris);
    if (inMesh.num_polys() < 1) return NULL;

    // 1) solid voxelization: voxels are marked as INSIDE, OUTSIDE or BOUNDARY
    cinolib::VoxelGrid grid;
    int resolution = std::max(8, opt.resolution);
    cinolib::voxelize(inMesh, (unsigned int)resolution, grid);

    const unsigned int dim[3] = { grid.dim[0], grid.dim[1], grid.dim[2] };
    const size_t numCorners = (size_t)(dim[0] + 1) * (dim[1] + 1) * (dim[2] + 1);

    // 2) scalar field on grid corners: 1 = solid, 0 = empty
    std::vector<unsigned char> cornerField(numCorners, 0);
    std::vector<cinolib::vec3d> cornerPos(numCorners);
    for (unsigned int k = 0; k < dim[2]; ++k)
    {
        unsigned int ijk[3] = { 0, 0, k };
        for (unsigned int j = 0; j < dim[1]; ++j)
        {
            ijk[1] = j;
            for (unsigned int i = 0; i < dim[0]; ++i)
            {
                ijk[0] = i;
                size_t index = cinolib::serialize_3D_index(i, j, k, dim[1], dim[2]);
                bool solid = (grid.voxels[index] &
                              (cinolib::VOXEL_INSIDE | cinolib::VOXEL_BOUNDARY)) != 0;

                // corner positions are needed for every corner of a mixed cell, so they are
                // always stored, while the field is only raised by solid voxels
                for (unsigned int off = 0; off < 8; ++off)
                {
                    unsigned int ci = cinolib::voxel_corner_index(grid, ijk, off);
                    cornerPos[ci] = cinolib::voxel_corner_xyz(grid, ijk, off);
                    if (solid) cornerField[ci] = 1;
                }
            }
        }
    }

    // 3) split every mixed cell into conforming tetrahedra; cells with a uniform field
    //    never contain the surface, so only mixed cells are tetrahedralized
    std::vector<std::vector<unsigned int>> globalTets;
    for (unsigned int k = 0; k < dim[2]; ++k)
    {
        unsigned int ijk[3] = { 0, 0, k };
        for (unsigned int j = 0; j < dim[1]; ++j)
        {
            ijk[1] = j;
            for (unsigned int i = 0; i < dim[0]; ++i)
            {
                ijk[0] = i;
                unsigned int corners[8];
                bool hasSolid = false, hasEmpty = false;
                for (unsigned int off = 0; off < 8; ++off)
                {
                    corners[off] = cinolib::voxel_corner_index(grid, ijk, off);
                    if (cornerField[corners[off]]) hasSolid = true; else hasEmpty = true;
                }
                if (!hasSolid || !hasEmpty) continue;

                std::vector<unsigned int> hex(corners, corners + 8);
                std::vector<unsigned int> tets;
                cinolib::hex_to_tets(hex, tets);
                std::vector<std::vector<unsigned int>> polys =
                    cinolib::polys_from_serialized_vids(tets, 4);
                globalTets.insert(globalTets.end(), polys.begin(), polys.end());
            }
        }
    }

    if (globalTets.empty())
    {
        OSG_WARN << "[osgVerse] MeshFixer::solidify(): no solid region found, try a higher "
                 << "resolution" << std::endl;
        return NULL;
    }

    // 4) build a tetrahedral mesh and store the inside/outside field in vertex UVW
    std::vector<cinolib::vec3d> tetVerts;
    std::vector<double> tetField;
    std::vector<int> globalToLocal(numCorners, -1);
    std::vector<std::vector<unsigned int>> tetPolys(globalTets.size());
    for (size_t i = 0; i < globalTets.size(); ++i)
    {
        tetPolys[i].resize(4);
        for (int c = 0; c < 4; ++c)
        {
            unsigned int g = globalTets[i][c];
            int& local = globalToLocal[g];
            if (local < 0)
            {
                local = (int)tetVerts.size();
                tetVerts.push_back(cornerPos[g]);
                tetField.push_back((double)cornerField[g]);
            }
            tetPolys[i][c] = (unsigned int)local;
        }
    }

    cinolib::Tetmesh<> tetMesh(tetVerts, tetPolys);
    for (size_t i = 0; i < tetVerts.size(); ++i)
        tetMesh.vert_data((unsigned int)i).uvw[0] = tetField[i];

    // 5) extract the isosurface with marching tets
    cinolib::Isosurface<> iso(tetMesh, 0.5f);
    cinolib::Trimesh<> outMesh = iso.export_as_trimesh();
    if (outMesh.num_polys() < 1)
    {
        OSG_WARN << "[osgVerse] MeshFixer::solidify(): isosurface extraction failed" << std::endl;
        return NULL;
    }

    // 6) smooth and reproject onto the input surface, removing the voxel stair-steps.
    //    A plain Laplacian step followed by a closest point reprojection is used instead of
    //    cinolib::mesh_smoother: the latter needs a sparse solve (slow for large outputs) and
    //    misbehaves when the smoothed mesh has flagged edges but the target has none.
    if (opt.smooth && opt.smoothIterations > 0)
    {
        cinolib::Octree targetTree;
        targetTree.build_from_mesh_polys(inMesh);

        std::vector<cinolib::vec3d> smoothed(outMesh.num_verts());
        for (int i = 0; i < opt.smoothIterations; ++i)
        {
            for (unsigned int vid = 0; vid < outMesh.num_verts(); ++vid)
            {
                const std::vector<unsigned int>& nbrs = outMesh.adj_v2v(vid);
                if (nbrs.empty()) { smoothed[vid] = outMesh.vert(vid); continue; }

                cinolib::vec3d pos(0.0, 0.0, 0.0);
                for (unsigned int nbr : nbrs) pos += outMesh.vert(nbr);
                smoothed[vid] = pos / (double)nbrs.size();
            }

            for (unsigned int vid = 0; vid < outMesh.num_verts(); ++vid)
                outMesh.vert(vid) = targetTree.closest_point(smoothed[vid]);
        }
    }

    if (info != NULL)
    {
        double minSJ = 1.0e30, sumSJ = 0.0;
        for (unsigned int pid = 0; pid < tetMesh.num_polys(); ++pid)
        {
            double sj = cinolib::tet_scaled_jacobian(tetMesh.poly_vert(pid, 0),
                                                     tetMesh.poly_vert(pid, 1),
                                                     tetMesh.poly_vert(pid, 2),
                                                     tetMesh.poly_vert(pid, 3));
            minSJ = std::min(minSJ, sj); sumSJ += sj;
        }
        info->gridSize[0] = (int)dim[0];
        info->gridSize[1] = (int)dim[1];
        info->gridSize[2] = (int)dim[2];
        info->numTetrahedra = (int)tetMesh.num_polys();
        info->numVertices = (int)outMesh.num_verts();
        info->numTriangles = (int)outMesh.num_polys();
        info->minScaledJacobian = (tetMesh.num_polys() > 0) ? minSJ : 0.0;
        info->avgScaledJacobian = (tetMesh.num_polys() > 0)
                                ? sumSJ / (double)tetMesh.num_polys() : 0.0;
    }

    std::vector<osg::Vec3> outVerts(outMesh.num_verts());
    for (unsigned int vid = 0; vid < outMesh.num_verts(); ++vid)
    {
        const cinolib::vec3d& p = outMesh.vert(vid);
        outVerts[vid] = osg::Vec3(p[0], p[1], p[2]);
    }

    std::vector<unsigned int> outIndices;
    outIndices.reserve(outMesh.num_polys() * 3);

    // make sure the surface is outward oriented, otherwise its lighting and any further
    // boolean operation on the result would be wrong
    double signedVolume = 0.0;
    for (unsigned int pid = 0; pid < outMesh.num_polys(); ++pid)
    {
        cinolib::vec3d p0 = outMesh.poly_vert(pid, 0), p1 = outMesh.poly_vert(pid, 1),
                       p2 = outMesh.poly_vert(pid, 2);
        signedVolume += p0.dot(p1.cross(p2));
    }
    const bool flipTriangles = (signedVolume < 0.0);

    for (unsigned int pid = 0; pid < outMesh.num_polys(); ++pid)
    {
        unsigned int i0 = outMesh.poly_vert_id(pid, 0), i1 = outMesh.poly_vert_id(pid, 1),
                     i2 = outMesh.poly_vert_id(pid, 2);
        outIndices.push_back(i0);
        outIndices.push_back(flipTriangles ? i2 : i1);
        outIndices.push_back(flipTriangles ? i1 : i2);
    }
    return createTriangleGeometry(outVerts, outIndices);
#else
    reportNotImplemented("solidify");
    return NULL;
#endif
}
