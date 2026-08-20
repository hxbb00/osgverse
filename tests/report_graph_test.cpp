#include <osg/io_utils>
#include <osg/ValueObject>
#include <osg/TriangleIndexFunctor>
#include <osg/Texture2D>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/MatrixTransform>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <VerseCommon.h>
#include <modeling/MeshTopology.h>
#include <modeling/Utilities.h>
#include <pipeline/IntersectionManager.h>
#include <ui/Utilities.h>
#include <iostream>
#include <sstream>

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
USE_SERIALIZER_WRAPPER(DracoGeometry)
#endif
USE_GRAPICSWINDOW_IMPLEMENTATION(SDL)
USE_GRAPICSWINDOW_IMPLEMENTATION(GLFW)

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

class Reporter : public osg::NodeVisitor
{
public:
    Reporter()
    :   osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
        _geomReporter(NULL), _textureReporter(NULL), _print(&osg::notify(osg::INFO)), _indent(0)
    { setNodeMaskOverride(0xffffffff); }

    virtual ~Reporter()
    { if (_geomReporter) delete _geomReporter; if (_textureReporter) delete _textureReporter; }

    void setVerbose(bool v) { _print = v ? &osg::notify(osg::WARN) : &osg::notify(osg::INFO); }

    void setGeometryReport(const std::string& file)
    {
        _geomReporter = new std::ofstream(file);
        (*_geomReporter) << "Class,Name,Primitives,Triangles,Vertices,Weldable,Attributes,"
                         << "Tex0,Tex1,Tex2,Tex3,Tex4,Tex5,Tex6,Tex7,Program" << std::endl;
    }

    void setTextureReport(const std::string& file)
    {
        _textureReporter = new std::ofstream(file);
        (*_textureReporter) << "TexClass,TexUnit,Loader,Name,Resolution,Bits,"
                            << "Components,Size(Mb),Compressed,Shared,FileName" << std::endl;
    }

    virtual void apply(osg::Node& node)
    {
        outputBasic(node, node.getNodeMask()); pushIndent();
        if (node.getStateSet()) apply(&node, NULL, *node.getStateSet());
        traverse(node); popIndent();
    }

    virtual void apply(osg::PagedLOD& node)
    {
        outputBasic(node, node.getNodeMask()); pushIndent();
        if (node.getStateSet()) apply(&node, NULL, *node.getStateSet());
        (*_print) << std::string(_indent, ' ') << "<Strategy> "
                  << (node.getCenterMode() == osg::LOD::USE_BOUNDING_SPHERE_CENTER ? "Child / " : "User / ")
                  << (node.getRangeMode() == osg::LOD::DISTANCE_FROM_EYE_POINT ? "Distance" : "Pixels");
        if (node.getCenterMode() == osg::LOD::USE_BOUNDING_SPHERE_CENTER) (*_print) << std::endl;
        else (*_print) << ", Bound = " << node.getCenter() << ", R = " << node.getRadius() << std::endl;

        unsigned int maxChild = node.getNumFileNames() - 1; traverse(node);
        for (unsigned int i = node.getNumChildren(); i <= maxChild; ++i)
        {
            std::string fileName = node.getDatabasePath() + node.getFileName(i);
            (*_print) << std::string(_indent, ' ') << "<File " << i << "/" << maxChild << ">: "
                      << fileName << ", Exist = " << !osgDB::findDataFile(fileName).empty() << "; Min = "
                      << node.getMinRange(i) << ", Max = " << node.getMaxRange(i) << std::endl;

            osg::ref_ptr<osg::Node> child = osgDB::readNodeFile(fileName);
            if (child.valid())
            {
                pushIndent(); outputBasic(*child, child->getNodeMask());
                (*_print) << std::string(_indent, ' ') << "<Bound> " << child->getBound().center()
                          << ", R = " << child->getBound().radius() << std::endl;;
                popIndent();
            }
        }
        popIndent();
    }

    virtual void apply(osg::ProxyNode& node)
    {
        outputBasic(node, node.getNodeMask()); pushIndent();
        if (node.getStateSet()) apply(&node, NULL, *node.getStateSet());

        unsigned int maxChild = node.getNumFileNames() - 1; traverse(node);
        for (unsigned int i = node.getNumChildren(); i <= maxChild; ++i)
        {
            std::string fileName = node.getDatabasePath() + node.getFileName(i);
            (*_print) << std::string(_indent, ' ') << "<File " << i << "/" << maxChild << ">: "
                      << fileName << ", Exist = " << !osgDB::findDataFile(fileName).empty() << std::endl;
        }
        popIndent();
    }

    virtual void apply(osg::Transform& node)
    {
        osg::Matrix matrix, matrixL;
        if (!_matrixStack.empty()) matrix = _matrixStack.back();
        outputBasic(node, node.getNodeMask()); pushIndent();
        node.computeLocalToWorldMatrix(matrix, this);
        node.computeLocalToWorldMatrix(matrixL, this);
        if (node.getStateSet()) apply(&node, NULL, *node.getStateSet());

        if (matrixL != matrix)
        {
            osg::Vec3 pos, scale; osg::Quat rot, so; matrixL.decompose(pos, rot, scale, so);
            osg::Vec3d euler = osgVerse::computeHPRFromQuat(rot);
            (*_print) << std::string(_indent, ' ') << "<Local>: ";
            if (!osg::equivalent(scale.length2(), 3.0f)) (*_print) << "S = " << scale << ", ";
            if (!rot.zeroRotation()) (*_print) << "R = " << euler << " (HPR), ";
            (*_print) << "T = " << pos << std::endl;
        }

        if (!matrix.isIdentity())
        {
            osg::Vec3 pos, scale; osg::Quat rot, so; matrix.decompose(pos, rot, scale, so);
            osg::Vec3d euler = osgVerse::computeHPRFromQuat(rot);
            (*_print) << std::string(_indent, ' ') << "<World>: ";
            if (!osg::equivalent(scale.length2(), 3.0f)) (*_print) << "S = " << scale << ", ";
            if (!rot.zeroRotation()) (*_print) << "R = " << euler << " (HPR), ";
            (*_print) << "T = " << pos << std::endl;
        }

        pushMatrix(matrix); traverse(node);
        popMatrix(); popIndent();
    }

    virtual void apply(osg::Geode& node)
    {
        outputBasic(node, node.getNodeMask()); pushIndent();
        if (node.getStateSet()) apply(&node, NULL, *node.getStateSet());
#if OSG_VERSION_LESS_OR_EQUAL(3, 4, 1)
        for (unsigned int i = 0; i < node.getNumDrawables(); ++i)
        {
            osg::Geometry* geom = node.getDrawable(i)->asGeometry();
            if (geom != NULL) apply(*geom);
        }
#endif
        traverse(node); popIndent();
    }

    virtual void apply(osg::Geometry& geom)
    {
        osg::Matrix matrix;
        if (_matrixStack.size() > 0) matrix = _matrixStack.back();
        outputBasic(geom, 1); pushIndent(); _imageMap.clear();

        osg::StateSet* ss = geom.getStateSet();
        if (ss) apply(geom.getNumParents() > 0 ? geom.getParent(0) : NULL, &geom, *ss);

        osgVerse::BoundingVolumeVisitor bvv; bvv.apply(geom);
        osgVerse::BoundingVolumeVisitor bvv1; bvv1.setWeldingVertices(true); bvv1.apply(geom);
        std::string nonManiType = "", attrTypes; unsigned int manifoldProblemID = 0;
        switch (bvv1.isManifold(manifoldProblemID))
        {
        case osgVerse::MeshCollector::IS_MANIFOLD: nonManiType = "Manifold"; break;
        case osgVerse::MeshCollector::NONMANIFOLD_VERTEX: nonManiType = "Non-Manifold Vertex"; break;
        case osgVerse::MeshCollector::UNCLOSED_MESH_BOUNDARY: nonManiType = "Unclosed Mesh"; break;
        case osgVerse::MeshCollector::SELF_INTERSECTION: nonManiType = "Self-Intersected Mesh"; break;
        case osgVerse::MeshCollector::FLIPPED_FACE_ORIENTATION: nonManiType = "Negative Face orientation"; break;
        case osgVerse::MeshCollector::NEGATIVE_VOLUME: nonManiType = "Negative Mesh"; break;
        default: break;
        }

        size_t numV = bvv.getVertices().size(), weldedV = bvv1.getVertices().size();
        (*_print) << std::string(_indent, ' ') << "<" << nonManiType << ">: "
                  << "Primitives = " << geom.getNumPrimitiveSets() << ", Triangles = "
                  << (bvv.getTriangles().size() / 3) << "; Vertices = " << numV << ",";
        if (!bvv.getAttributes(osgVerse::MeshCollector::NormalAttr).empty()) attrTypes += " +N";
        if (!bvv.getAttributes(osgVerse::MeshCollector::ColorAttr).empty()) attrTypes += " +C";
        for (size_t u = 0; u < geom.getNumTexCoordArrays(); ++u) attrTypes += " +UV" + std::to_string(u);
        (*_print) << attrTypes << "; Weldable = " << (float)(numV - weldedV) / (float)numV << std::endl;

        if (_geomReporter && _geomSet.find(&geom) == _geomSet.end())
        {
            (*_geomReporter) << geom.libraryName() << "::" << geom.className() << "," << geom.getName()
                             << "," << geom.getNumPrimitiveSets() << "," << (bvv.getTriangles().size() / 3)
                             << "," << numV << "," << (numV - weldedV) << "," << attrTypes << ",";
            for (int i = 0; i < 8; ++i)
            {
                if (_imageMap.find(i) == _imageMap.end()) { (*_geomReporter) << ","; continue; }
                osg::Image* img = (_imageMap[i]->getNumImages() > 0) ? _imageMap[i]->getImage(0) : NULL;
                if (img) (*_geomReporter) << img->s() << "x" << img->t(); (*_geomReporter) << ",";
            }

            osg::Program* prog = static_cast<osg::Program*>(
                ss ? ss->getAttribute(osg::StateAttribute::PROGRAM) : NULL);
            if (prog) (*_geomReporter) << prog->getName() << " (" << prog->getNumShaders() << "),";
            else (*_geomReporter) << ","; (*_geomReporter) << std::endl;
            _geomSet.insert(&geom);
        }
#if OSG_VERSION_GREATER_THAN(3, 4, 1)
        traverse(geom);
#endif
        popIndent();
    }

    virtual void apply(osg::Node* n, osg::Drawable* d, osg::StateSet& ss)
    {
        (*_print) << std::string(_indent, ' ') << "<StateSet> " << ss.getName();
        if (ss.referenceCount() == 1) (*_print) << std::endl;
        else (*_print) << " (SHARED x" << ss.referenceCount() << ")" << std::endl;
        pushIndent();

        osg::StateSet::AttributeList& attrList = ss.getAttributeList();
        for (osg::StateSet::AttributeList::iterator itr = attrList.begin();
             itr != attrList.end(); ++itr)
        {
            osg::StateAttribute::Type t = itr->first.first;
            (*_print) << std::string(_indent, ' ') << "<" << itr->second.first->className()
                      << "> " << itr->second.first->getName() << std::endl;
        }

        osg::StateSet::TextureAttributeList& texAttrList = ss.getTextureAttributeList();
        for (size_t i = 0; i < texAttrList.size(); ++i)
        {
            osg::StateSet::AttributeList& attr = texAttrList[i];
            for (osg::StateSet::AttributeList::iterator itr = attr.begin();
                itr != attr.end(); ++itr)
            {
                osg::StateAttribute::Type t = itr->first.first;
                (*_print) << std::string(_indent, ' ') << "<" << itr->second.first->className()
                          << ", Unit-" << i << "> " << itr->second.first->getName();
                if (t != osg::StateAttribute::TEXTURE) { (*_print) << std::endl; continue; }
                
                osg::Texture* tex = static_cast<osg::Texture*>(itr->second.first.get());
                if (tex->referenceCount() == 1) (*_print) << std::endl;
                else (*_print) << " (SHARED x" << tex->referenceCount() << ")" << std::endl;

                pushIndent();
                for (size_t k = 0; k < tex->getNumImages(); ++k)
                {
                    osg::Image* image = tex->getImage(k);
                    (*_print) << std::string(_indent, ' ') << "<" << image->className() <<"> "
                              << image->getName() << ": Resolution = " << image->s() << "x" << image->t()
                              << "x" << image->r() << ", DDS = " << image->isCompressed()
                              << ", Translucent = " << image->isImageTranslucent();
                    if (!image->getFileName().empty()) (*_print) << ", " << image->getFileName();
                    if (image->referenceCount() == 1) (*_print) << std::endl;
                    else (*_print) << " (SHARED x" << image->referenceCount() << ")" << std::endl;

                    if (_textureReporter && _imageSet.find(image) == _imageSet.end())
                    {
                        std::string loader; image->getUserValue("Loader", loader);
                        unsigned int comp = osg::Image::computeNumComponents(image->getPixelFormat());
                        unsigned int bitsAll = osg::Image::computePixelSizeInBits(image->getPixelFormat(), image->getDataType());
                        float size = image->getTotalSizeInBytes() / 1024.0f / 1024.0f;
                        (*_textureReporter) << tex->className() << "," << i << "," << loader << "," << image->getName() << ","
                                            << image->s() << "x" << image->t() << "x" << image->r() << ","
                                            << (bitsAll / comp) << "," << comp << "," << size << "," << image->isCompressed() << ","
                                            << (image->referenceCount() - 1) << "," << image->getFileName() << std::endl;
                        _imageSet.insert(image);
                    }
                }
                _imageMap[i] = tex; popIndent();
            }
        }
        popIndent();
    }

protected:
    inline void pushIndent() { _indent += 2; }
    inline void popIndent() { if (_indent > 0) _indent -= 2; }
    inline void pushMatrix(const osg::Matrix& matrix) { _matrixStack.push_back(matrix); }
    inline void popMatrix() { _matrixStack.pop_back(); }

    inline void outputBasic(osg::Object& obj, unsigned int mask)
    {
        (*_print) << std::string(_indent, ' ') << (mask == 0 ? "**HIDED**[" : "[") << obj.libraryName()
                  << "::" << obj.className() << "] " << osgVerse::getNodePathID(obj) << ": ";
#if OSG_VERSION_GREATER_THAN(3, 3, 0)
        osg::Geode* geode = (obj.asNode() != NULL) ? obj.asNode()->asGeode() : NULL;
#else
        osg::Geode* geode = dynamic_cast<osg::Geode*>(&obj);
#endif
        if (geode)
            (*_print) << "Drawables = " << geode->getNumDrawables();
        else
        {
#if OSG_VERSION_GREATER_THAN(3, 3, 0)
            osg::Group* group = (obj.asNode() != NULL) ? obj.asNode()->asGroup() : NULL;
#else
            osg::Group* group = dynamic_cast<osg::Group*>(&obj);
#endif
            if (group) (*_print) << "Children = " << group->getNumChildren();
        }
        if (obj.referenceCount() == 1) (*_print) << std::endl;
        else (*_print) << " (SHARED x" << obj.referenceCount() << ")" << std::endl;
    }

    typedef std::vector<osg::Matrix> MatrixStack;
    MatrixStack _matrixStack;
    std::map<int, osg::Texture*> _imageMap;
    std::set<osg::Geometry*> _geomSet;
    std::set<osg::Image*> _imageSet;
    std::ostream* _geomReporter;
    std::ostream* _textureReporter;
    std::ostream* _print;
    int _indent;
};

class InteractiveHandler : public osgGA::GUIEventHandler
{
public:
    typedef osgVerse::HeadUpDisplayCanvas::ChildLayout ChildLayout;
    typedef osgVerse::HeadUpDisplayCanvas::Anchor Anchor;
    InteractiveHandler(osg::Group* root, osgVerse::HeadUpDisplayCanvas* h) : _canvas(h)
    {
        _intersectShowNode = new osg::Geode; rebuildIntersectionShowNode();
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
        _intersectShowNode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#endif
        root->addChild(_intersectShowNode.get());

        _canvas->createText("main", L"Ctrl+click to add intersection line to the model",
                            32, 800, 40, "root", ChildLayout::FREE, Anchor::BOTTOM);
        _canvas->createText("button", L"Clear", 16, 100, 40, "root", ChildLayout::FREE, Anchor::BOTTOM | Anchor::RIGHT);
        _canvas->setAsButton("button", [&]() { rebuildIntersectionShowNode(); });
    }

    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
        if (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE &&
            (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL))
        {
            osgVerse::IntersectionResult result = osgVerse::findNearestIntersection(
                view->getCamera(), ea.getXnormalized(), ea.getYnormalized());
            drawIntersectionResult(view->getCamera(), ea, result);
        }
        return false;
    }

protected:
    void rebuildIntersectionShowNode()
    {
        _intersectLines = osgVerse::createGeometry(
            new osg::Vec3Array, new osg::Vec3Array, osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f), new osg::DrawArrays(GL_LINES));
        _intersectShowNode->removeDrawables(0, _intersectShowNode->getNumDrawables());
        _intersectShowNode->addDrawable(_intersectLines.get());
    }

    void drawIntersectionResult(osg::Camera* cam, const osgGA::GUIEventAdapter& ea,
                                osgVerse::IntersectionResult& result)
    {
        osg::Vec3Array* va = static_cast<osg::Vec3Array*>(_intersectLines->getVertexArray());
        osg::Vec3Array* na = static_cast<osg::Vec3Array*>(_intersectLines->getNormalArray());
        osg::Vec4Array* ca = static_cast<osg::Vec4Array*>(_intersectLines->getColorArray());
        va->dirty(); na->dirty(); ca->dirty(); _intersectLines->dirtyBound();

        osg::Matrix invMVP = osg::Matrix::inverse(cam->getViewMatrix() * cam->getProjectionMatrix());
        osg::Vec3d start(ea.getXnormalized(), ea.getYnormalized(), -1.0f);
        if (result.drawable.valid())
        {
            va->push_back(start * invMVP); va->push_back(result.getWorldIntersectPoint());
            na->insert(na->end(), 2, osg::Vec3(0.0f, 0.0f, 1.0f));
            ca->insert(ca->end(), 2, osg::Vec4(1.0f, 1.0f, 0.0f, 1.0f));
            _canvas->texts["main"]->setText(osgVerse::getNodePathID(*result.drawable));
        }
        else
        {
            va->push_back(start * invMVP);
            va->push_back(osg::Vec3d(ea.getXnormalized(), ea.getYnormalized(), 1.0f) * invMVP);
            na->insert(na->end(), 2, osg::Vec3(0.0f, 0.0f, 1.0f));
            ca->insert(ca->end(), 2, osg::Vec4(1.0f, 0.0f, 0.0f, 1.0f));
            _canvas->texts["main"]->setText("No intersection");
        }

        osg::DrawArrays* p = static_cast<osg::DrawArrays*>(_intersectLines->getPrimitiveSet(0));
        p->setCount(va->size()); p->dirty();
    }

    osg::ref_ptr<osg::Geode> _intersectShowNode;
    osg::ref_ptr<osg::Geometry> _intersectLines;
    osgVerse::HeadUpDisplayCanvas* _canvas;
};

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    osg::ref_ptr<osg::Node> scene =
        (argc < 2) ? osgDB::readNodeFile("cessna.osg") : osgDB::readNodeFiles(arguments);
    if (!scene) { OSG_WARN << "Failed to load " << (argc < 2) ? "" : argv[1]; return 1; }

    Reporter reporter;
    reporter.setVerbose(!arguments.read("--slient"));
    reporter.setGeometryReport("geom_report.csv");
    reporter.setTextureReport("texture_report.csv");
    scene->accept(reporter);

    // Start the main loop
    osgVerse::HeadUpDisplayCanvas hudCanvas;
    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
    root->addChild(hudCanvas.create(1920, 1080));
    root->addChild(scene.get());
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
    root->getOrCreateStateSet()->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
    root->getOrCreateStateSet()->addUniform(new osg::Uniform("baseTexture", (int)0));
#endif

    osgViewer::Viewer viewer;
    viewer.addEventHandler(new InteractiveHandler(root.get(), &hudCanvas));
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    return viewer.run();
}
