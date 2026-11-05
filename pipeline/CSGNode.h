#ifndef MANA_PP_CSGNODE_HPP
#define MANA_PP_CSGNODE_HPP

#include <osg/Group>
#include <osg/BoundingSphere>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Stencil>
#include <osg/NodeCallback>
#include <osg/Referenced>
#include <string>
#include <vector>

namespace osgUtil { class CullVisitor; }

namespace osgVerse
{
    /** Boolean operation a CSG primitive takes part in */
    enum CSGOperation
    {
        CSG_INTERSECTION = 0,
        CSG_SUBTRACTION  = 1
    };

    /** Order in which the batches of subtracted primitives are processed */
    enum CSGSequencingMode
    {
        /** Shortest known permutation embedding sequence: O(n^2) passes but no knowledge
            about the depth complexity is required */
        CSG_SCHOENFIELD_SEQUENCING = 0,
        /** Bouncing sequence: fewer passes when the depth complexity is known, see
            CSGNode::setDepthComplexity() */
        CSG_BOUNCING_SEQUENCING    = 1
    };

    /** Render state of one pass of the SCS algorithm, a 1:1 mapping of the glDepth*,
        glStencil*, glCullFace and glColorMask calls of the original OpenCSG implementation */
    struct CSGPassState
    {
        CSGPassState();

        bool colorWrite;  ///< write to the color buffer?
        osg::Depth::Function depthFunc;
        bool depthWrite;
        bool useDepthRange;
        double depthRangeNear, depthRangeFar;

        bool stencilTest;
        osg::Stencil::Function stencilFunc;
        int stencilRef;
        unsigned int stencilFuncMask;
        osg::Stencil::Operation stencilFailOp, stencilDepthFailOp, stencilDepthPassOp;
        unsigned int stencilWriteMask;

        int cullFace;  ///< 0 (disabled), GL_FRONT or GL_BACK

        /// The final pass which shades the result with the primitives' own materials,
        /// i.e. which does not override the program of the geometry
        bool shadingPass;

        void setDepth(osg::Depth::Function func, bool write);
        void setDepthRange(double zNear, double zFar);
        void setStencil(osg::Stencil::Function func, int ref, unsigned int funcMask,
                        osg::Stencil::Operation sfail, osg::Stencil::Operation zfail,
                        osg::Stencil::Operation zpass, unsigned int writeMask = 0xff);
    };

    class CSGNode;

    /** A CSG primitive: geometry plus the boolean operation it takes part in.
        Any osg::Node can be used (a Geode, a Geometry/ShapeDrawable, a MatrixTransform
        or a whole subtree); it is added as a child of the CSGNode */
    class CSGPrimitive : public osg::Referenced
    {
    public:
        CSGPrimitive(osg::Node* geometry = NULL, CSGOperation op = CSG_INTERSECTION);

        void setGeometry(osg::Node* geometry) { _geometry = geometry; }
        osg::Node* getGeometry() { return _geometry.get(); }
        const osg::Node* getGeometry() const { return _geometry.get(); }

        void setOperation(CSGOperation op) { _operation = op; }
        CSGOperation getOperation() const { return _operation; }

    protected:
        virtual ~CSGPrimitive();

        osg::ref_ptr<osg::Node> _geometry;
        CSGOperation _operation;
    };

    /** Renderer of the SCS algorithm of OpenCSG (http://www.opencsg.org), which computes
        the boolean combination (intersection minus subtractions) of convex primitives in
        the depth and stencil buffers, and then shades the resulting surface with the
        primitives' own materials.

        The default implementation renders into the frame buffer which is current during
        the cull traversal, restricted by a scissor rectangle (the screen space bound of
        the intersected primitives). Subclasses can override the low-level target
        operations (beginComputation()/clearBuffers()/drawPrimitives()/drawScreenQuad()/
        endComputation()) to render into an offscreen target instead, while reusing the
        pass sequence implemented by render(). */
    class CSGNodeRenderer : public osg::Referenced
    {
    public:
        enum Buffer
        {
            DEPTH_BUFFER   = 1,
            STENCIL_BUFFER = 2
        };

        CSGNodeRenderer();

        /** Computes the boolean result of the node and emits all passes needed for it.
            The depth and stencil buffers contain the surface of the result on return */
        virtual void render(osgUtil::CullVisitor& cv, CSGNode& node);

        /** Invoked once per computation, before any pass is emitted.
            @param primitives primitives of the Intersection operation */
        virtual void beginComputation(osgUtil::CullVisitor& cv,
                                      const std::vector<CSGPrimitive*>& primitives);

        /** Invoked once per computation, after all passes have been emitted */
        virtual void endComputation(osgUtil::CullVisitor& cv);

        /** Clear the given buffers of the target in the current region */
        virtual void clearBuffers(osgUtil::CullVisitor& cv, unsigned int buffers,
                                  const CSGPassState& state);

        /** Emit one pass drawing the given primitives with the given state */
        virtual void drawPrimitives(osgUtil::CullVisitor& cv,
                                    const std::vector<CSGPrimitive*>& primitives,
                                    const CSGPassState& state);

        /** Emit one pass drawing a full screen quad with the given state. The quad is used
            to reset the depth buffer (depth range [0,0]) at selected fragments */
        virtual void drawScreenQuad(osgUtil::CullVisitor& cv, const CSGPassState& state);

        /** Number of passes emitted by the last computation */
        unsigned int getPassCount() const { return _passCount; }

        /** Vertex shader used by the passes computing the boolean result. It must transform
            the vertices in exactly the same way as the final shading does. An empty string
            selects the default "gl_Position = VERSE_MATRIX_MVP * osg_Vertex;" */
        void setVertexShader(const std::string& code) { _vertexShader = code; }
        const std::string& getVertexShader() const { return _vertexShader; }

    protected:
        virtual ~CSGNodeRenderer();

        struct RenderData;  // frame buffer workspace, defined in CSGNodeRenderer.cpp
        RenderData& getRenderData();

        osg::StateSet* preparePassState(osgUtil::CullVisitor& cv, const CSGPassState& state,
                                        bool screenQuad);
        osg::Depth* getDepthAttribute(const CSGPassState& state);
        osg::Stencil* getStencilAttribute(const CSGPassState& state);
        osg::CullFace* getCullFaceAttribute(int face);
        osg::Program* getPassProgram(bool screenQuad);
        osg::Geometry* getQuadGeometry();
        osg::Drawable* getClearDrawable(unsigned int glBuffers);

        std::string _vertexShader;
        unsigned int _passCount;
        RenderData* _renderData;

    private:
        CSGNodeRenderer(const CSGNodeRenderer&);
        CSGNodeRenderer& operator=(const CSGNodeRenderer&);
    };

    /** Node displaying the CSG (boolean) result of its primitives.

        Requirements / notes:
        - the frame buffer needs a stencil buffer, e.g.
          osg::DisplaySettings::instance()->setMinimumNumStencilBits(8);
        - every primitive must describe ONE closed, convex and manifold solid with counter
          clockwise winding (front faces pointing outwards); concave or disjoint parts have
          to be added as separate primitives
        - the algorithm resets depth and stencil values inside the screen space bounding box
          of the primitives of the Intersection operation. Fragments which are not part of
          the result are marked with the depth value of the near plane, so that they can
          never be matched by the shading pass: the CSG node should therefore be drawn
          before other opaque geometry covering the same screen region
        - the compute passes transform vertices with the default vertex shader; if the
          geometry is drawn with a different transformation, set that shader with
          setVertexShader() so that the computed depth matches the final shading
        - nested osg::Camera, nested CSGNode, instanced drawables and shaders doing their
          own vertex transformation (without setVertexShader()) are not supported */
    class CSGNode : public osg::Group
    {
    public:
        CSGNode();
        CSGNode(const CSGNode& rhs, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
        META_Node(osgVerse, CSGNode)

        /** Add a primitive; the geometry is added as a child of this node */
        void addPrimitive(osg::Node* geometry, CSGOperation op = CSG_INTERSECTION);
        void addPrimitive(CSGPrimitive* primitive);

        unsigned int getNumPrimitives() const { return (unsigned int)_primitives.size(); }
        CSGPrimitive* getPrimitive(unsigned int i);
        const CSGPrimitive* getPrimitive(unsigned int i) const;
        CSGPrimitive* getPrimitive(const osg::Node* geometry);

        /** Remove the primitive, but keep its geometry as a child */
        void removePrimitive(unsigned int i);
        /** Remove all primitives, but keep their geometries as children */
        void clearPrimitives();

        void setSequencingMode(CSGSequencingMode mode) { _sequencingMode = mode; }
        CSGSequencingMode getSequencingMode() const { return _sequencingMode; }

        /** Depth complexity used by CSG_BOUNCING_SEQUENCING, i.e. the maximum number of
            surfaces of the subtracted primitives at a single ray. 0 (default) means
            unknown, in which case CSG_SCHOENFIELD_SEQUENCING is used */
        void setDepthComplexity(unsigned int dc) { _depthComplexity = dc; }
        unsigned int getDepthComplexity() const { return _depthComplexity; }

        /** Set to true if the camera may be inside the CSG model. Disables a performance
            optimization of the subtraction phase */
        void setCameraInsideModel(bool b) { _cameraInsideModel = b; }
        bool getCameraInsideModel() const { return _cameraInsideModel; }

        /** Draw the resulting surface with the primitives' own materials after the boolean
            operation has been computed (default: true) */
        void setShadingPassEnabled(bool b) { _shadingPassEnabled = b; }
        bool getShadingPassEnabled() const { return _shadingPassEnabled; }

        /** Number of stencil bits of the frame buffer (default 8). Only used to decide when
            the stencil buffer has to be cleared while cycling the reference values */
        void setStencilBits(unsigned int bits) { _stencilBits = bits ? bits : 8; }
        unsigned int getStencilBits() const { return _stencilBits; }

        void setVertexShader(const std::string& code);
        const std::string& getVertexShader() const { return _vertexShader; }

        /** Set the renderer the passes are emitted to. NULL (default) uses an internal
            CSGNodeRenderer which renders into the current frame buffer. Note that a
            renderer caches its pass states per render stage, so it must not be shared by
            several CSG nodes which are drawn into the same render stage */
        void setRenderer(CSGNodeRenderer* renderer);
        CSGNodeRenderer* getRenderer() { return _renderer.get(); }
        const CSGNodeRenderer* getRenderer() const { return _renderer.get(); }

        /** Number of passes of the last cull traversal (statistics/debugging) */
        unsigned int getLastPassCount() const { return _lastPassCount; }

        /** Bounding sphere of the primitives of the Intersection operation in local
            coordinates (the region the computation can be restricted to) */
        osg::BoundingSphere getIntersectedBound() const;

        /** Emits the passes of the SCS algorithm (called by the cull callback) */
        void cull(osgUtil::CullVisitor& cv);

    protected:
        virtual ~CSGNode();

        typedef std::vector<osg::ref_ptr<CSGPrimitive>> PrimitiveList;
        osg::ref_ptr<CSGPrimitive> findPrimitive(const osg::Node* geometry) const;

        PrimitiveList _primitives;
        CSGSequencingMode _sequencingMode;
        unsigned int _depthComplexity;
        bool _cameraInsideModel, _shadingPassEnabled;
        unsigned int _stencilBits;
        std::string _vertexShader;
        osg::ref_ptr<CSGNodeRenderer> _renderer;
        osg::ref_ptr<osg::NodeCallback> _cullCallback;
        unsigned int _lastPassCount;
    };
}

#endif
