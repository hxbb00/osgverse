#include <osg/Notify>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <OpenThreads/Mutex>
#include "CSGNode.h"

using namespace osgVerse;

namespace
{
    /// Cull callback emitting the passes of the CSG algorithm. The cull traversal is
    /// completely replaced: the children of the CSG node are only drawn by the passes
    /// which are emitted for them (which is why the callback does not call traverse())
    class CSGCullCallback : public osg::NodeCallback
    {
    public:
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            osgUtil::CullVisitor* cv = nv ? nv->asCullVisitor() : NULL;
            if (cv != NULL && cv->getCurrentRenderStage() != NULL)
                static_cast<CSGNode*>(node)->cull(*cv);
            else
                node->traverse(*nv);  // update, compute bounds, ...: behave like a group
        }
    };

    /// The passes are emitted during the cull traversal and modify the state of the
    /// (single) renderer of a node; serialize the computations in case several cameras
    /// are culled in parallel
    OpenThreads::Mutex& getCullMutex()
    {
        static OpenThreads::Mutex mutex;
        return mutex;
    }
}

CSGPrimitive::CSGPrimitive(osg::Node* geometry, CSGOperation op)
{
    _geometry = geometry; _operation = op;
}

CSGPrimitive::~CSGPrimitive()
{}

CSGNode::CSGNode()
{
    _sequencingMode = CSG_SCHOENFIELD_SEQUENCING;
    _depthComplexity = 0; _cameraInsideModel = false; _shadingPassEnabled = true;
    _stencilBits = 8; _lastPassCount = 0;
    setDataVariance(osg::Object::DYNAMIC);
    setName("osgVerse::CSGNode");

    _cullCallback = new CSGCullCallback;
    setCullCallback(_cullCallback.get());
}

CSGNode::CSGNode(const CSGNode& rhs, const osg::CopyOp& copyop)
:   osg::Group(rhs, copyop)
{
    _sequencingMode = rhs._sequencingMode;
    _depthComplexity = rhs._depthComplexity;
    _cameraInsideModel = rhs._cameraInsideModel;
    _shadingPassEnabled = rhs._shadingPassEnabled;
    _stencilBits = rhs._stencilBits; _vertexShader = rhs._vertexShader;
    _lastPassCount = 0;
    _cullCallback = new CSGCullCallback;
    setCullCallback(_cullCallback.get());

    // The geometries of the copied node are the (copied) children; the renderer is not
    // copied and a default one will be created on demand
    for (unsigned int i = 0; i < rhs._primitives.size(); ++i)
    {
        const CSGPrimitive* primitive = rhs._primitives[i].get();
        const osg::Node* sourceGeometry = primitive->getGeometry();
        osg::Node* geometry = NULL;

        unsigned int index = rhs.getChildIndex(sourceGeometry);
        if (index < getNumChildren()) geometry = getChild(index);
        _primitives.push_back(new CSGPrimitive(geometry, primitive->getOperation()));
    }
}

CSGNode::~CSGNode()
{}

void CSGNode::addPrimitive(osg::Node* geometry, CSGOperation op)
{
    addPrimitive(new CSGPrimitive(geometry, op));
}

void CSGNode::addPrimitive(CSGPrimitive* primitive)
{
    if (primitive == NULL) return;
    _primitives.push_back(primitive);

    osg::Node* geometry = primitive->getGeometry();
    if (geometry != NULL && getChildIndex(geometry) >= getNumChildren())
        addChild(geometry);
}

CSGPrimitive* CSGNode::getPrimitive(unsigned int i)
{
    return (i < _primitives.size()) ? _primitives[i].get() : NULL;
}

const CSGPrimitive* CSGNode::getPrimitive(unsigned int i) const
{
    return (i < _primitives.size()) ? _primitives[i].get() : NULL;
}

osg::ref_ptr<CSGPrimitive> CSGNode::findPrimitive(const osg::Node* geometry) const
{
    for (unsigned int i = 0; i < _primitives.size(); ++i)
    {
        if (_primitives[i]->getGeometry() == geometry)
            return _primitives[i];
    }
    return NULL;
}

CSGPrimitive* CSGNode::getPrimitive(const osg::Node* geometry)
{
    return findPrimitive(geometry).get();
}

void CSGNode::removePrimitive(unsigned int i)
{
    if (i >= _primitives.size()) return;

    osg::Node* geometry = _primitives[i]->getGeometry();
    _primitives.erase(_primitives.begin() + i);
    if (geometry != NULL && !findPrimitive(geometry).valid())
    {
        unsigned int index = getChildIndex(geometry);
        if (index < getNumChildren()) removeChild(index);
    }
}

void CSGNode::clearPrimitives()
{
    for (unsigned int i = 0; i < _primitives.size(); ++i)
    {
        osg::Node* geometry = _primitives[i]->getGeometry();
        if (geometry == NULL) continue;

        unsigned int index = getChildIndex(geometry);
        if (index < getNumChildren()) removeChild(index);
    }
    _primitives.clear();
}

void CSGNode::setVertexShader(const std::string& code)
{
    _vertexShader = code;
    if (_renderer.valid()) _renderer->setVertexShader(code);
}

void CSGNode::setRenderer(CSGNodeRenderer* renderer)
{
    _renderer = renderer;
    if (_renderer.valid()) _renderer->setVertexShader(_vertexShader);
}

osg::BoundingSphere CSGNode::getIntersectedBound() const
{
    osg::BoundingSphere result;
    for (unsigned int i = 0; i < _primitives.size(); ++i)
    {
        const CSGPrimitive* primitive = _primitives[i].get();
        if (primitive->getOperation() != CSG_INTERSECTION) continue;

        const osg::Node* geometry = primitive->getGeometry();
        if (geometry != NULL) result.expandBy(geometry->getBound());
    }
    return result;
}

void CSGNode::cull(osgUtil::CullVisitor& cv)
{
    if (_primitives.empty()) return;
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock(getCullMutex());

    if (!_renderer.valid())
    {
        CSGNodeRenderer* renderer = new CSGNodeRenderer;
        renderer->setVertexShader(_vertexShader);
        _renderer = renderer;
    }

    _renderer->render(cv, *this);
    _lastPassCount = _renderer->getPassCount();
}
