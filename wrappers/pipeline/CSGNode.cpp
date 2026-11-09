#include <osg/io_utils>
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>
#include <pipeline/CSGNode.h>
using namespace osgVerse;

static bool checkSequencingMode(const CSGNode& node)
{ return node.getSequencingMode() != CSG_SCHOENFIELD_SEQUENCING; }

static bool readSequencingMode(osgDB::InputStream& is, CSGNode& node)
{
    int mode = 0; is >> mode;
    node.setSequencingMode((CSGSequencingMode)mode); return true;
}

static bool writeSequencingMode(osgDB::OutputStream& os, const CSGNode& node)
{
    os << (int)node.getSequencingMode() << std::endl; return true;
}

static bool checkPrimitiveList(const CSGNode& node) { return node.getNumPrimitives() > 0; }

static bool readPrimitiveList(osgDB::InputStream& is, CSGNode& node)
{
    unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
    for (unsigned int i = 0; i < size; ++i)
    {
        int childIndex = -1, operation = 0;
        is >> is.PROPERTY("Primitive") >> childIndex >> operation;
        if (childIndex >= 0 && (unsigned int)childIndex < node.getNumChildren())
            node.addPrimitive(node.getChild((unsigned int)childIndex), (CSGOperation)operation);
    }
    is >> is.END_BRACKET;
    return true;
}

static bool writePrimitiveList(osgDB::OutputStream& os, const CSGNode& node)
{
    os.writeSize(node.getNumPrimitives()); os << os.BEGIN_BRACKET << std::endl;
    for (unsigned int i = 0; i < node.getNumPrimitives(); ++i)
    {
        const CSGPrimitive* primitive = node.getPrimitive(i);
        const osg::Node* geom = primitive->getGeometry();
        int childIndex = (geom != NULL) ? (int)node.getChildIndex(geom) : -1;
        os << os.PROPERTY("Primitive") << childIndex << (int)primitive->getOperation() << std::endl;
    }
    os << os.END_BRACKET << std::endl;
    return true;
}

REGISTER_OBJECT_WRAPPER(CSGNode,
                        new osgVerse::CSGNode,
                        osgVerse::CSGNode,
                        "osg::Object osg::Node osg::Group osgVerse::CSGNode")
{
    ADD_USER_SERIALIZER(SequencingMode);
    ADD_UINT_SERIALIZER(DepthComplexity, 0);
    ADD_BOOL_SERIALIZER(CameraInsideModel, false);
    ADD_BOOL_SERIALIZER(ShadingPassEnabled, true);
    ADD_UINT_SERIALIZER(StencilBits, 8);
    ADD_STRING_SERIALIZER(VertexShader, "");
    ADD_USER_SERIALIZER(PrimitiveList);
}
