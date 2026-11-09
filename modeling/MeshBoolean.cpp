#include <osg/Array>
#include <osg/Geometry>
#include <osg/Math>
#include <osgUtil/SmoothingVisitor>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include "Utilities.h"
#include "MeshBoolean.h"

#ifdef VERSE_WITH_CINOLIB
#   include "3rdparty/InteractiveMeshBool/InteractiveMeshBool.h"
#endif

using namespace osgVerse;

namespace
{
    struct EdgeInfo
    {
        EdgeInfo() : count(0), dirSum(0) {}
        int count;   // number of triangles using this undirected edge
        int dirSum;  // +1/-1 per directed use, should cancel for a consistent orientation
    };

    inline uint64_t makeEdgeKey(unsigned int a, unsigned int b)
    {
        unsigned int lo = a < b ? a : b, hi = a < b ? b : a;
        return ((uint64_t)lo << 32) | (uint64_t)hi;
    }
}

MeshBoolean::MeshBoolean(Operation op)
:   _operation(op), _weldVertices(true), _smoothNormals(true),
    _validateInput(true), _creaseAngle(30.0f)
{
}

MeshBoolean::~MeshBoolean()
{
}

bool MeshBoolean::isSupported()
{
#ifdef VERSE_WITH_CINOLIB
    return true;
#else
    return false;
#endif
}

bool MeshBoolean::collect(osg::Node* node, std::vector<osg::Vec3>& vertices,
                          std::vector<unsigned int>& triangles)
{
    if (node == NULL)
    {
        _error = "Input node is null";
        return false;
    }

    MeshCollector collector;
    collector.setWeldingVertices(_weldVertices);
    collector.setOnlyVertexAndIndices(true);
    node->accept(collector);

    vertices = collector.getVertices();
    triangles = collector.getTriangles();
    if (vertices.empty() || triangles.empty())
    {
        _error = "Input node contains no triangle data";
        return false;
    }
    return true;
}

bool MeshBoolean::validateMesh(const std::vector<osg::Vec3>& vertices,
                               const std::vector<unsigned int>& triangles, const char* name)
{
    if (vertices.size() < 4 || triangles.size() < 12 || (triangles.size() % 3) != 0)
    {
        _error = std::string(name) + " is not a valid closed triangle mesh";
        return false;
    }

    std::unordered_map<uint64_t, EdgeInfo> edges;
    edges.reserve(triangles.size());
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
    {
        unsigned int i0 = triangles[i + 0], i1 = triangles[i + 1], i2 = triangles[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            _error = std::string(name) + " contains an out-of-range vertex index";
            return false;
        }
        if (i0 == i1 || i1 == i2 || i0 == i2)
        {
            _error = std::string(name) + " contains a degenerate triangle";
            return false;
        }

        const unsigned int tri[3] = { i0, i1, i2 };
        for (int e = 0; e < 3; ++e)
        {
            unsigned int a = tri[e], b = tri[(e + 1) % 3];
            EdgeInfo& info = edges[makeEdgeKey(a, b)];
            ++info.count;
            info.dirSum += (a < b) ? 1 : -1;
        }
    }

    for (std::unordered_map<uint64_t, EdgeInfo>::const_iterator it = edges.begin();
         it != edges.end(); ++it)
    {
        if (it->second.count != 2)
        {
            _error = std::string(name) + " is not a closed manifold mesh (boundary or non-manifold edge)";
            return false;
        }
        if (it->second.dirSum != 0)
        {
            _error = std::string(name) + " has inconsistent triangle orientation";
            return false;
        }
    }
    return true;
}

osg::Geometry* MeshBoolean::compute(osg::Node* nodeA, osg::Node* nodeB)
{
    _error.clear();
    if (!isSupported())
    {
        _error = "MeshBoolean is unavailable: build without cinolib/InteractiveMeshBool";
        return NULL;
    }

    std::vector<osg::Vec3> verticesA, verticesB;
    std::vector<unsigned int> trianglesA, trianglesB;
    if (!collect(nodeA, verticesA, trianglesA)) return NULL;
    if (!collect(nodeB, verticesB, trianglesB)) return NULL;
    return compute(verticesA, trianglesA, verticesB, trianglesB);
}

osg::Geometry* MeshBoolean::compute(const std::vector<osg::Vec3>& verticesA,
                                    const std::vector<unsigned int>& trianglesA,
                                    const std::vector<osg::Vec3>& verticesB,
                                    const std::vector<unsigned int>& trianglesB)
{
    _error.clear();
    if (!isSupported())
    {
        _error = "MeshBoolean is unavailable: build without cinolib/InteractiveMeshBool";
        return NULL;
    }

    if (_validateInput)
    {
        if (!validateMesh(verticesA, trianglesA, "Operand A")) return NULL;
        if (!validateMesh(verticesB, trianglesB, "Operand B")) return NULL;
    }

    std::vector<double> coords;
    coords.reserve((verticesA.size() + verticesB.size()) * 3);
    for (size_t i = 0; i < verticesA.size(); ++i)
    { coords.push_back(verticesA[i].x()); coords.push_back(verticesA[i].y()); coords.push_back(verticesA[i].z()); }
    for (size_t i = 0; i < verticesB.size(); ++i)
    { coords.push_back(verticesB[i].x()); coords.push_back(verticesB[i].y()); coords.push_back(verticesB[i].z()); }

    std::vector<uint32_t> tris, labels;
    tris.reserve(trianglesA.size() + trianglesB.size());
    labels.reserve(trianglesA.size() / 3 + trianglesB.size() / 3);
    for (size_t i = 0; i < trianglesA.size(); ++i) tris.push_back(trianglesA[i]);
    for (size_t i = 0; i < trianglesB.size(); ++i) tris.push_back((uint32_t)verticesA.size() + trianglesB[i]);
    for (size_t i = 0; i < trianglesA.size() / 3; ++i) labels.push_back(0);
    for (size_t i = 0; i < trianglesB.size() / 3; ++i) labels.push_back(1);

    std::vector<double> outCoords;
    std::vector<uint32_t> outTris;
#ifdef VERSE_WITH_CINOLIB
    imb::booleanPipeline(coords, tris, labels, static_cast<imb::BooleanOp>(_operation),
                         outCoords, outTris);
#else
    _error = "MeshBoolean is unavailable: build without cinolib/InteractiveMeshBool";
    return NULL;
#endif
    if (outCoords.empty() || outTris.empty())
    {
        _error = "Boolean operation produced an empty mesh";
        return NULL;
    }
    return buildGeometry(outCoords, outTris);
}

osg::Geometry* MeshBoolean::buildGeometry(const std::vector<double>& coords,
                                          const std::vector<uint32_t>& triangles)
{
    if ((coords.size() % 3) != 0 || triangles.empty() || (triangles.size() % 3) != 0)
    {
        _error = "Boolean kernel returned invalid mesh data";
        return NULL;
    }

    osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
    va->reserve(coords.size() / 3);
    for (size_t i = 0; i < coords.size(); i += 3)
        va->push_back(osg::Vec3((float)coords[i], (float)coords[i + 1], (float)coords[i + 2]));

    osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
    de->resize(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i) (*de)[i] = triangles[i];

    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
    geom->setVertexArray(va.get());
    geom->addPrimitiveSet(de.get());
    geom->setUseDisplayList(false);
    geom->setUseVertexBufferObjects(true);

    // Keep sharp boolean edges hard while smoothing curved input surfaces. The vertex array
    // may be split by the smoothing visitor, so create the color array afterwards.
    if (_smoothNormals)
        osgUtil::SmoothingVisitor::smooth(*geom, osg::DegreesToRadians(_creaseAngle));
    osg::ref_ptr<osg::Vec4Array> ca = new osg::Vec4Array;
    ca->assign(geom->getVertexArray()->getNumElements(), osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
    geom->setColorArray(ca.get(), osg::Array::BIND_PER_VERTEX);

    geom->setName("MeshBooleanResult");
    return geom.release();
}
