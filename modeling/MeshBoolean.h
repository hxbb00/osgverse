#ifndef MANA_MODELING_MESHBOOLEAN_HPP
#define MANA_MODELING_MESHBOOLEAN_HPP

#include <osg/Geometry>
#include <osg/Node>
#include <osg/Referenced>
#include <cstdint>
#include <string>
#include <vector>

namespace osgVerse
{
    /** Boolean operations on two closed manifold triangle meshes.

        The input nodes are collected as world-space triangle soups (their transforms are
        baked), and the result is a new world-space osg::Geometry. The implementation uses
        InteractiveMeshBool; both operands must be closed and consistently oriented, which
        is validated before the operation. The underlying library is not re-entrant, so do
        not call compute() from several threads at the same time.

        Example:
          osgVerse::MeshBoolean mb(osgVerse::MeshBoolean::Difference);
          osg::ref_ptr<osg::Geometry> result = mb.compute(nodeA, nodeB);
          if (!result.valid()) std::cout << mb.getLastError() << std::endl;
    */
    class MeshBoolean : public osg::Referenced
    {
    public:
        enum Operation
        {
            Union = 0,          ///< A ∪ B
            Intersection = 1,   ///< A ∩ B
            Difference = 2,     ///< A - B
            Xor = 3             ///< symmetric difference
        };

        MeshBoolean(Operation op = Difference);
        virtual ~MeshBoolean();
        void setOperation(Operation op) { _operation = op; }
        Operation getOperation() const { return _operation; }

        /** Weld coincident vertices while collecting the input nodes (default true).
            Keep it enabled unless the input meshes already share vertices exactly. */
        void setWeldingVertices(bool b) { _weldVertices = b; }
        bool getWeldingVertices() const { return _weldVertices; }

        /** Generate smoothed normals on the result (default true). A crease angle keeps
            sharp boolean edges hard while smoothing curved surfaces. */
        void setSmoothNormals(bool b) { _smoothNormals = b; }
        bool getSmoothNormals() const { return _smoothNormals; }
        void setCreaseAngle(float deg) { _creaseAngle = deg; }
        float getCreaseAngle() const { return _creaseAngle; }

        /** Reject inputs which are not closed, edge-manifold and consistently oriented
            (default true). Disabling this may make the kernel fail or crash. */
        void setValidateInput(bool b) { _validateInput = b; }
        bool getValidateInput() const { return _validateInput; }

        /** Compute the boolean of two nodes. Returns a new geometry, or NULL on failure;
            see getLastError() for the reason. The input nodes are not modified. */
        osg::Geometry* compute(osg::Node* nodeA, osg::Node* nodeB);

        /** Compute the boolean of two explicit triangle soups. Vertices are expected in the
            same coordinate space; callers that work with transformed nodes should use the
            node overload above. */
        osg::Geometry* compute(const std::vector<osg::Vec3>& verticesA,
                               const std::vector<unsigned int>& trianglesA,
                               const std::vector<osg::Vec3>& verticesB,
                               const std::vector<unsigned int>& trianglesB);

        /** Last error message (empty after a successful call). */
        const std::string& getLastError() const { return _error; }

        /** True when this build has InteractiveMeshBool/cinolib enabled. */
        static bool isSupported();

    protected:
        bool collect(osg::Node* node, std::vector<osg::Vec3>& vertices,
                     std::vector<unsigned int>& triangles);
        bool validateMesh(const std::vector<osg::Vec3>& vertices,
                          const std::vector<unsigned int>& triangles, const char* name);
        osg::Geometry* buildGeometry(const std::vector<double>& coords,
                                     const std::vector<uint32_t>& triangles);

        Operation _operation;
        bool _weldVertices, _smoothNormals, _validateInput;
        float _creaseAngle;
        std::string _error;
    };
}

#endif
