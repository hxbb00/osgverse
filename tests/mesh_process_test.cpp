#include <osg/io_utils>
#include <osg/PolygonMode>
#include <osg/LineWidth>
#include <osg/MatrixTransform>
#include <osg/Geometry>
#include <osg/ShapeDrawable>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgGA/StateSetManipulator>
#include <osgUtil/SmoothingVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <modeling/DynamicGeometry.h>
#include <modeling/MeshBoolean.h>
#include <modeling/MeshTopology.h>
#include <modeling/GeometryMapper.h>
#include <modeling/MeshFixer.h>
#include <modeling/Predicates.h>
#include <modeling/Utilities.h>
#include <modeling/Math.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <iostream>
#include <sstream>
#include <utility>

#define TEST_MAPPING_TO_VHACD 0
#define TEST_MODELING_FUNCTIONS 1

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

static osg::Program* createUnlitProgram()
{
    const char* vsCode = {
        "VERSE_VS_OUT vec4 color;\n"
        "void main() {\n"
        "    color = osg_Color;\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* fsCode = {
        "VERSE_FS_IN vec4 color;\n"
        "VERSE_FS_OUT vec4 fragData;\n"
        "void main() {\n"
        "    fragData = color;\n"
        "    VERSE_FS_FINAL(fragData);\n"
        "}\n"
    };

    int cxtVer = 0, glslVer = 0;
#if defined(OSG_GL3_AVAILABLE)
    cxtVer = 330; glslVer = 330;  // use core-profile attribute/uniform names
#elif defined(OSG_GLES3_AVAILABLE)
    cxtVer = 300; glslVer = 300;
#else
    osgVerse::guessOpenGLVersions(cxtVer, glslVer);
#endif
    osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, vsCode);
    osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, fsCode);
    osgVerse::Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
    osgVerse::Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(vs.get()); program->addShader(fs.get());
    return program.release();
}

static osg::Node* createBooleanDemo(osgVerse::MeshBoolean::Operation op)
{
    // A large box with a sphere subtracted from its top face; the resulting crater is much
    // more visible than a small box/box difference. Tessellation hints follow csg_node_test:
    // only positions are needed by the boolean kernel.
    osg::ref_ptr<osg::TessellationHints> hints = new osg::TessellationHints;
    hints->setDetailRatio(1.0f);
    hints->setCreateNormals(false);
    hints->setCreateTextureCoords(false);

    osg::ref_ptr<osg::Geode> geodeA = new osg::Geode;
    geodeA->addDrawable(new osg::ShapeDrawable(
        new osg::Box(osg::Vec3(0.0f, 0.0f, 0.0f), 8.0f, 8.0f, 6.0f), hints.get()));
    osg::ref_ptr<osg::Geode> geodeB = new osg::Geode;
    geodeB->addDrawable(new osg::ShapeDrawable(
        new osg::Sphere(osg::Vec3(0.0f, 0.0f, 4.5f), 3.0f), hints.get()));

    osgVerse::MeshBoolean boolean(op);
    osg::ref_ptr<osg::Geometry> result = boolean.compute(geodeA.get(), geodeB.get());
    if (!result.valid())
    {
        OSG_WARN << "[MeshBoolean] Demo failed: " << boolean.getLastError() << std::endl;
        return NULL;
    }

    // Give the result a visible color; MeshBoolean outputs white so that pipelines which
    // multiply texture by vertex color do not render it black
    osg::Vec4Array* colors = dynamic_cast<osg::Vec4Array*>(result->getColorArray());
    if (colors != NULL)
        for (osg::Vec4Array::iterator it = colors->begin(); it != colors->end(); ++it)
            *it = osg::Vec4(1.0f, 0.35f, 0.1f, 1.0f);

    // Add a dark wireframe overlay sharing the same arrays, so the spherical cut and the new
    // intersection edges are readable even with flat/unlit shading
    osg::ref_ptr<osg::Geometry> wire = new osg::Geometry(*result, osg::CopyOp::SHALLOW_COPY);
    wire->setStateSet(new osg::StateSet);
    osg::ref_ptr<osg::Vec4Array> wireColors = new osg::Vec4Array;
    wireColors->assign(result->getVertexArray()->getNumElements(), osg::Vec4(0.05f, 0.05f, 0.05f, 1.0f));
    wire->setColorArray(wireColors.get(), osg::Array::BIND_PER_VERTEX);
    osg::StateSet* wireSS = wire->getOrCreateStateSet();
    wireSS->setAttributeAndModes(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK,
                                                      osg::PolygonMode::LINE));
    wireSS->setAttribute(new osg::LineWidth(1.5f));

    std::cout << "MeshBoolean: " << result->getVertexArray()->getNumElements() << "V, "
              << result->getNumPrimitiveSets() << " primitive set(s)" << std::endl;

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(result.get());
    geode->addDrawable(wire.get());
    geode->getOrCreateStateSet()->setAttributeAndModes(createUnlitProgram());
    return geode.release();
}

// Collect a node as a welded triangle soup
static void collectSoup(osg::Node& node, std::vector<osg::Vec3>& verts,
                        std::vector<unsigned int>& tris)
{
    osgVerse::MeshCollector collector;
    collector.setWeldingVertices(true);
    collector.setOnlyVertexAndIndices(true);
    node.accept(collector);
    verts = collector.getVertices(); tris = collector.getTriangles();
}

// Build a geometry from a triangle soup without reordering its vertices
static osg::Geometry* createSoupGeometry(const std::vector<osg::Vec3>& verts,
                                         const std::vector<unsigned int>& tris)
{
    osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) (*va)[i] = verts[i];

    osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
    de->assign(tris.begin(), tris.end());
    return osgVerse::createGeometry(va.get(), NULL, NULL, de.get(), true, true, false);
}

// Create a small sphere marker with an overall color, used for inside/outside probes
static osg::Geometry* createMarker(const osg::Vec3& pos, const osg::Vec4& color)
{
    osg::ref_ptr<osg::Geometry> geom = osgVerse::createGeodesicSphere(pos, 0.3f, 1);
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    colors->push_back(color);
    geom->setColorArray(colors.get(), osg::Array::BIND_OVERALL);
    return geom.release();
}

// Attach a node to the scene with an unlit program and a transform
static osg::MatrixTransform* addDemoNode(osg::Group* root, osg::Node* node,
                                         const osg::Matrix& matrix)
{
    osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
    mt->setMatrix(matrix); mt->addChild(node);
    node->getOrCreateStateSet()->setAttributeAndModes(createUnlitProgram());
    root->addChild(mt.get());
    return mt.release();
}

int main(int argc, char** argv)
{
    osg::ref_ptr<osg::Node> scene =
        (argc < 2) ? osgDB::readNodeFile("cessna.osg") : osgDB::readNodeFile(argv[1]);
    if (!scene) { OSG_WARN << "Failed to load " << (argc < 2) ? "" : argv[1]; return 1; }

    osgVerse::MeshTopologyVisitor mtv;
    mtv.setWeldingVertices(true); scene->accept(mtv);

    osg::ref_ptr<osgVerse::MeshTopology> topology = mtv.generate();
    //topology->simplify(0.8f);
    unsigned int problemID = 0; int result = mtv.isManifold(problemID);
    std::cout << "Input is manifold? " << result << "; Problem = " << problemID << "\n";

    std::vector<std::vector<uint32_t>> entities = topology->getEntityFaces();
#if 0
    for (size_t i = 0; i < entities.size(); ++i)
    {
        std::vector<uint32_t>& faces = entities[i];
        if (faces.size() > 40) continue;

        for (size_t j = 0; j < faces.size(); ++j)
            topology->deleteFace(faces[j]);
    }
    topology->prune();
#endif

#if !TEST_MAPPING_TO_VHACD
    static osg::Vec4 colors[] = {
        osg::Vec4(0.8f, 0.8f, 0.8f, 1.0f), osg::Vec4(1.0f, 0.0f, 0.0f, 1.0f),
        osg::Vec4(0.0f, 1.0f, 0.0f, 1.0f), osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f),
        osg::Vec4(1.0f, 1.0f, 0.0f, 1.0f), osg::Vec4(0.0f, 1.0f, 1.0f, 1.0f),
        osg::Vec4(1.0f, 0.0f, 1.0f, 1.0f), osg::Vec4(0.2f, 0.2f, 0.2f, 1.0f)
    };

    // Core-profile friendly entity coloring program. The previous fragment-only shader used
    // gl_FragColor / gl_Color, which do not exist in a GL3/GLES core context: it failed to
    // compile and link, and the entity object ended up with a wrong position and color.
    const char* vsCode = {
        "void main() {\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* fragCode = {
        "uniform vec4 color;\n"
        "VERSE_FS_OUT vec4 fragData;\n"
        "void main() {\n"
        "    fragData = color;\n"
        "    VERSE_FS_FINAL(fragData);\n"
        "}\n"
    };
    int cxtVer = 0, glslVer = 0;
#if defined(OSG_GL3_AVAILABLE)
    cxtVer = 330; glslVer = 330;
#elif defined(OSG_GLES3_AVAILABLE)
    cxtVer = 300; glslVer = 300;
#else
    osgVerse::guessOpenGLVersions(cxtVer, glslVer);
#endif
    osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, vsCode);
    osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, fragCode);
    osgVerse::Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
    osgVerse::Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(vs.get()); program->addShader(fs.get());

    osg::ref_ptr<osg::MatrixTransform> topoMT = new osg::MatrixTransform;
    {   // Test topology entities
        osg::ref_ptr<osg::Geode> topoGeode = topology->outputByEntity();
        for (size_t i = 0; i < topoGeode->getNumDrawables(); ++i)
        {
            std::cout << "Entity-" << i << ": Faces = " << entities[i].size() << std::endl;
            topoGeode->getDrawable(i)->getOrCreateStateSet()->addUniform(
                new osg::Uniform("color", colors[i % 8]));
        }
        topoGeode->setStateSet(mtv.getMergedStateSet());
        topoGeode->getOrCreateStateSet()->setAttributeAndModes(program.get());
        //osgDB::writeNodeFile(*topoGeode, "topoResult.osg");

        topoMT->addChild(topoGeode.get());
        topoMT->setMatrix(osg::Matrix::translate(0.0f, 0.0f, 20.0f));
    }

    osg::ref_ptr<osg::MatrixTransform> sdfMT = new osg::MatrixTransform;
    {   // Test SDF generation
        osgVerse::SDFGridCreator sgc; osgVerse::SDFGridCreator::SDF sdfData;
        sgc.setWeldingVertices(true); scene->accept(sgc);

        osg::ref_ptr<osg::Geode> sdfGeode = new osg::Geode;
        if (sgc.generate(sdfData, 50, 50, 50))
        {
            std::vector<osg::Vec3> samples = sgc.sampleVolume(sdfData, 0.1f);
            osg::Geometry* sdfGeom = osgVerse::createGeometry(
                new osg::Vec3Array(samples.begin(), samples.end()), NULL, osg::Vec4(1.0f, 1.0f, 0.0f, 1.0f),
                new osg::DrawArrays(GL_POINTS, 0, samples.size()));
            
            sdfGeode->addDrawable(sdfGeom);
#  if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
            sdfGeode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#  endif
            sdfMT->addChild(sdfGeode.get());
        }
        sdfMT->setMatrix(osg::Matrix::translate(0.0f, 0.0f, -20.0f));
    }
#endif

    osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform;
#if TEST_MAPPING_TO_VHACD
    osgVerse::BoundingVolumeVisitor bvv; scene->accept(bvv);
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(bvv.computeVHACD());
    root->addChild(geode.get());
    osgUtil::SmoothingVisitor smv; geode->accept(smv);

    osgVerse::GeometryMapper mapper;
    std::cout << "Similarity: " << mapper.computeSimilarity(scene.get(), geode.get()) << std::endl;
    mapper.mapAttributes(scene.get(), geode.get());

    osg::ref_ptr<osg::MatrixTransform> sceneMT = new osg::MatrixTransform;
    sceneMT->setMatrix(osg::Matrix::translate(0.0f, 0.0f, 20.0f));
    sceneMT->addChild(scene.get());
    root->addChild(sceneMT.get());
#else
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
#  if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) && !defined(OSG_GL3_AVAILABLE)
    geode->getOrCreateStateSet()->setAttributeAndModes(
        new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE));
    geode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
#  endif
    root->addChild(geode.get());

    // Test VHACD
    osgVerse::BoundingVolumeVisitor bvv; scene->accept(bvv);
    geode->addDrawable(bvv.computeVHACD());
    root->addChild(topoMT.get());
    root->addChild(sdfMT.get());
#endif

#if TEST_MODELING_FUNCTIONS
    {
        std::vector<osg::Vec3d> pathL;
        pathL.push_back(osg::Vec3(2.0f, 0.0f, 5.0f));
        pathL.push_back(osg::Vec3(4.0f, 0.0f, 4.0f));
        pathL.push_back(osg::Vec3(6.0f, 0.0f, 2.0f));
        pathL.push_back(osg::Vec3(7.0f, 0.0f, 0.0f));
        osg::ref_ptr<osg::Geometry> geomL =
            osgVerse::createLatheGeometry(pathL, osg::Z_AXIS, 16, true);

        std::vector<osg::Vec3d> pathE;
        std::vector<std::vector<osg::Vec3d>> pathEinner;
        pathE.push_back(osg::Vec3(10.0f, 0.0f, 0.0f));
        pathE.push_back(osg::Vec3(15.0f, 0.0f, 0.0f));
        pathE.push_back(osg::Vec3(15.0f, 5.0f, 0.0f));
        pathE.push_back(osg::Vec3(10.0f, 5.0f, 0.0f));
        {
            std::vector<osg::Vec3d> pathI0;
            pathI0.push_back(osg::Vec3(11.0f, 1.0f, 0.0f));
            pathI0.push_back(osg::Vec3(14.0f, 1.0f, 0.0f));
            pathI0.push_back(osg::Vec3(14.0f, 4.0f, 0.0f));
            pathI0.push_back(osg::Vec3(12.0f, 4.0f, 0.0f));
            pathEinner.push_back(pathI0);
        }
        osg::ref_ptr<osg::Geometry> geomE =
            osgVerse::createExtrusionGeometry(pathE, pathEinner, osg::Z_AXIS * 5.0f, true);

        std::vector<osg::Vec3d> pathLo;
        std::vector<std::vector<osg::Vec3d>> sectionsLo;
        pathLo.push_back(osg::Vec3(20.0f, 0.0f, 0.0f));
        pathLo.push_back(osg::Vec3(25.0f, 10.0f, 0.0f));
        pathLo.push_back(osg::Vec3(25.0f, 10.0f, 10.0f));
        pathLo.push_back(osg::Vec3(30.0f, 5.0f, 5.0f));
        {
            std::vector<osg::Vec3d> sec0;
            sec0.push_back(osg::Vec3(-1.0f, -1.0f, 0.0f));
            sec0.push_back(osg::Vec3(1.0f, -1.0f, 0.0f));
            sec0.push_back(osg::Vec3(1.0f, 1.0f, 0.0f));
            sec0.push_back(osg::Vec3(-1.0f, 1.0f, 0.0f));
            sectionsLo.push_back(sec0);
        }
        osg::ref_ptr<osg::Geometry> geomLo =
            osgVerse::createLoftGeometry(pathLo, sectionsLo, true, true);

        osg::ref_ptr<osg::Geode> geode1 = new osg::Geode;
        geode1->getOrCreateStateSet()->setTextureAttributeAndModes(
            0, osgVerse::createTexture2D(osgDB::readImageFile(BASE_DIR + "/textures/uv.jpg")));
        geode1->addDrawable(geomL.get());
        geode1->addDrawable(geomE.get());
        geode1->addDrawable(geomLo.get());
        //osgDB::writeNodeFile(*geode1, "test_mesh.osg");

#if true  // check manifold
        osgVerse::MeshCollector mc; unsigned int problemID = 0;
        mc.setWeldingVertices(true); mc.setUseGlobalVertices(true);
        geomLo->accept(mc); int result = mc.isManifold(problemID);
        std::cout << "INPUT: " << mc.getVertices().size() << "V; " << (mc.getTriangles().size() / 3) << "P\n";
        std::cout << "Manifold: " << result << "; Problem = " << problemID << "\n";
#endif

        osg::ref_ptr<osg::MatrixTransform> mt1 = new osg::MatrixTransform;
        mt1->setMatrix(osg::Matrix::translate(20.0f, 0.0f, 0.0f));
        mt1->addChild(geode1.get());
        root->addChild(mt1.get());
    }
#endif

    // Test mesh boolean (InteractiveMeshBool): large box - sphere
    {
        osg::ref_ptr<osg::Node> booleanNode = createBooleanDemo(osgVerse::MeshBoolean::Difference);
        if (booleanNode.valid())
        {
            osg::ref_ptr<osg::MatrixTransform> mt2 = new osg::MatrixTransform;
            mt2->setMatrix(osg::Matrix::translate(-20.0f, 0.0f, 0.0f));
            mt2->addChild(booleanNode.get());
            root->addChild(mt2.get());
        }
    }

    // Test new modeling helpers (cinolib based): predicates, inside/outside test,
    // self-intersection detection, non-manifold repair, connected components,
    // solidification and feature line extraction
    if (osgVerse::MeshFixer::isSupported())
    {
        // (1) Robust geometric predicates: values and signs are printed
        {
            osg::Vec2d a(0.0, 0.0), b(1.0, 0.0), c(0.0, 1.0), d(0.2, 0.2), e(5.0, 5.0);
            osg::Vec3d q0(0.0, 0.0, 0.0), q1(1.0, 0.0, 0.0), q2(2.0, 0.0, 0.0),
                       q3(0.0, 1.0, 0.0), q4(0.0, 0.0, 1.0);
            std::cout << "[Predicates] orient2D = " << osgVerse::orient2D(a, b, c) << " (CCW) / "
                      << osgVerse::orient2D(b, a, c) << " (CW)\n";
            std::cout << "[Predicates] inCircle = " << osgVerse::inCircle(a, b, c, d)
                      << " (inside) / " << osgVerse::inCircle(a, b, c, e) << " (outside)\n";
            std::cout << "[Predicates] colinear(q0,q1,q2) = "
                      << osgVerse::arePointsColinear(q0, q1, q2)
                      << ", colinear(q0,q1,q3) = " << osgVerse::arePointsColinear(q0, q1, q3)
                      << ", coplanar(q0,q1,q2,q3) = "
                      << osgVerse::arePointsCoplanar(q0, q1, q2, q3)
                      << ", coplanar(q0,q1,q3,q4) = "
                      << osgVerse::arePointsCoplanar(q0, q1, q3, q4) << std::endl;
        }

        // (2) A self-intersecting triangle soup: two overlapping spheres
        std::vector<osg::Vec3> soupVerts; std::vector<unsigned int> soupTris;
        {
            osg::ref_ptr<osg::TessellationHints> hints = new osg::TessellationHints;
            hints->setDetailRatio(0.6f);
            osg::ref_ptr<osg::Geode> soupGeode = new osg::Geode;
            soupGeode->addDrawable(new osg::ShapeDrawable(
                new osg::Sphere(osg::Vec3(0.0f, 0.0f, 0.0f), 3.0f), hints.get()));
            soupGeode->addDrawable(new osg::ShapeDrawable(
                new osg::Sphere(osg::Vec3(3.0f, 0.0f, 0.0f), 3.0f), hints.get()));
            collectSoup(*soupGeode, soupVerts, soupTris);
        }
        osg::ref_ptr<osg::Geometry> soupGeom = createSoupGeometry(soupVerts, soupTris);
        {
            osgVerse::MeshCollector soupChecker; unsigned int soupProblem = 0;
            soupChecker.setWeldingVertices(true); soupChecker.apply(*soupGeom);
            std::cout << "[MeshFixer] input soup: manifold = "
                      << soupChecker.isManifold(soupProblem) << " (problem " << soupProblem
                      << ")" << std::endl;
        }

        // (3) Self-intersection detection: the offending triangles are painted red
        {
            std::vector<std::pair<unsigned int, unsigned int>> pairs;
            unsigned int num = osgVerse::MeshFixer::findSelfIntersections(*soupGeom, pairs);
            std::cout << "[MeshFixer] " << soupTris.size() / 3 << " triangles, found " << num
                      << " self-intersecting pair(s)" << std::endl;

            std::vector<bool> bad(soupTris.size() / 3, false);
            for (size_t i = 0; i < pairs.size(); ++i)
                { bad[pairs[i].first] = true; bad[pairs[i].second] = true; }

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->resize(soupVerts.size(), osg::Vec4(0.75f, 0.75f, 0.75f, 1.0f));
            for (size_t i = 0; i < bad.size(); ++i)
            {
                if (!bad[i]) continue;
                for (int k = 0; k < 3; ++k)
                    (*colors)[soupTris[i * 3 + k]] = osg::Vec4(1.0f, 0.1f, 0.1f, 1.0f);
            }
            soupGeom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);

            osg::ref_ptr<osg::Geode> soupGeode = new osg::Geode;
            soupGeode->addDrawable(soupGeom.get());
            addDemoNode(root.get(), soupGeode.get(), osg::Matrix::translate(45.0f, 0.0f, 0.0f));
        }

        // (4) Solidification: turn the self-intersecting soup into a watertight surface,
        //     then test a few probe points with winding numbers
        {
            osgVerse::MeshFixer::SolidifyOptions opt;
            opt.resolution = 40;
            osgVerse::MeshFixer::SolidifyInfo info;
            osg::ref_ptr<osg::Geometry> solid =
                osgVerse::MeshFixer::solidify(*soupGeom, opt, &info);
            if (solid.valid())
            {
                std::cout << "[MeshFixer] solidify: grid " << info.gridSize[0] << "x"
                          << info.gridSize[1] << "x" << info.gridSize[2] << ", "
                          << info.numTetrahedra << " tets -> " << info.numVertices << "V / "
                          << info.numTriangles << "P, SJ = " << info.minScaledJacobian
                          << " / " << info.avgScaledJacobian << " (min/avg)" << std::endl;

                osgVerse::MeshCollector solidChecker; unsigned int solidProblem = 0;
                solidChecker.setWeldingVertices(true); solidChecker.apply(*solid);
                std::cout << "[MeshFixer] solidified surface: manifold = "
                          << solidChecker.isManifold(solidProblem) << " (problem "
                          << solidProblem << ")" << std::endl;

                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(osg::Vec4(1.0f, 0.55f, 0.15f, 1.0f));
                solid->setColorArray(colors.get(), osg::Array::BIND_OVERALL);

                osg::ref_ptr<osg::Geode> solidGeode = new osg::Geode;
                solidGeode->addDrawable(solid.get());
                osg::Vec3 probes[4] = {
                    osg::Vec3(0.0f, 0.0f, 0.0f), osg::Vec3(3.0f, 0.0f, 0.0f),
                    osg::Vec3(0.0f, 0.0f, 2.4f), osg::Vec3(0.0f, 0.0f, 4.0f)
                };
                for (int i = 0; i < 4; ++i)
                {
                    int wn = 0;
                    bool inside = osgVerse::isPointInsideMesh(solid.get(), probes[i], &wn);
                    std::cout << "[InsideTest] (" << probes[i].x() << ", " << probes[i].y()
                              << ", " << probes[i].z() << "): winding = " << wn
                              << ", inside = " << inside << std::endl;
                    solidGeode->addDrawable(createMarker(
                        probes[i], inside ? osg::Vec4(0.1f, 1.0f, 0.2f, 1.0f)
                                          : osg::Vec4(1.0f, 0.1f, 0.1f, 1.0f)));
                }
                addDemoNode(root.get(), solidGeode.get(),
                            osg::Matrix::translate(45.0f, 0.0f, 22.0f));
            }
        }

        // (5) Non-manifold repair: three triangles sharing one edge
        {
            osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
            va->push_back(osg::Vec3(0.0f, 0.0f, -1.0f));    // v0
            va->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));     // v1
            va->push_back(osg::Vec3(1.5f, 0.0f, 0.0f));     // apex 0
            va->push_back(osg::Vec3(-0.75f, 1.3f, 0.0f));   // apex 1
            va->push_back(osg::Vec3(-0.75f, -1.3f, 0.0f));  // apex 2
            osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
            const unsigned int fan[9] = { 0, 1, 2, 1, 0, 3, 0, 1, 4 };
            de->assign(fan, fan + 9);
            osg::ref_ptr<osg::Geometry> fanGeom =
                osgVerse::createGeometry(va.get(), NULL, NULL, de.get(), true, true, false);

            osgVerse::MeshCollector before; unsigned int fanProblem = 0;
            before.setWeldingVertices(true); before.apply(*fanGeom);
            std::cout << "[MeshFixer] non-manifold fan: " << before.getVertices().size()
                      << "V, manifold = " << before.isManifold(fanProblem) << " (problem "
                      << fanProblem << ")" << std::endl;

            osg::ref_ptr<osg::Geometry> fixed =
                osgVerse::MeshFixer::fixNonManifoldEdges(*fanGeom);
            if (fixed.valid())
            {
                osgVerse::MeshCollector after; fanProblem = 0;
                after.setWeldingVertices(false); after.apply(*fixed);
                std::cout << "[MeshFixer] after repair: " << after.getVertices().size()
                          << "V, manifold = " << after.isManifold(fanProblem) << " (problem "
                          << fanProblem << ")" << std::endl;

                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(osg::Vec4(0.2f, 0.9f, 1.0f, 1.0f));
                fixed->setColorArray(colors.get(), osg::Array::BIND_OVERALL);

                osg::ref_ptr<osg::Geode> fixedGeode = new osg::Geode;
                fixedGeode->addDrawable(fixed.get());
                addDemoNode(root.get(), fixedGeode.get(),
                            osg::Matrix::scale(4.0f, 4.0f, 4.0f) *
                            osg::Matrix::translate(45.0f, 0.0f, -22.0f));
            }
            else
                std::cout << "[MeshFixer] non-manifold fan: nothing repaired" << std::endl;
        }

        // (6) Connected components: two far apart spheres merged in one geometry
        {
            std::vector<osg::Vec3> compVerts; std::vector<unsigned int> compTris;
            {
                osg::ref_ptr<osg::TessellationHints> hints = new osg::TessellationHints;
                hints->setDetailRatio(0.4f);
                osg::ref_ptr<osg::Geode> compGeode = new osg::Geode;
                compGeode->addDrawable(new osg::ShapeDrawable(
                    new osg::Sphere(osg::Vec3(0.0f, 0.0f, 0.0f), 2.0f), hints.get()));
                compGeode->addDrawable(new osg::ShapeDrawable(
                    new osg::Sphere(osg::Vec3(8.0f, 0.0f, 0.0f), 2.0f), hints.get()));
                collectSoup(*compGeode, compVerts, compTris);
            }
            osg::ref_ptr<osg::Geometry> compGeom = createSoupGeometry(compVerts, compTris);

            std::vector<osg::ref_ptr<osg::Geometry>> parts =
                osgVerse::MeshFixer::splitConnectedComponents(*compGeom);
            std::cout << "[MeshFixer] split " << compVerts.size() << "V mesh into "
                      << parts.size() << " component(s)" << std::endl;

            osg::ref_ptr<osg::Group> partGroup = new osg::Group;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(osg::Vec4(0.2f + 0.4f * (float)i, 0.8f - 0.3f * (float)i,
                                            0.4f, 1.0f));
                parts[i]->setColorArray(colors.get(), osg::Array::BIND_OVERALL);

                osg::ref_ptr<osg::Geode> partGeode = new osg::Geode;
                partGeode->addDrawable(parts[i].get());
                osg::ref_ptr<osg::MatrixTransform> partMT = new osg::MatrixTransform;
                partMT->setMatrix(osg::Matrix::translate(0.0f, 0.0f, (float)i * 6.0f));
                partMT->addChild(partGeode.get()); partGroup->addChild(partMT.get());
            }
            if (!parts.empty())
                addDemoNode(root.get(), partGroup.get(),
                            osg::Matrix::translate(0.0f, 0.0f, -45.0f));
        }

        // (7) Feature line extraction: sharp edges of a box work as centerlines
        {
            osg::ref_ptr<osg::TessellationHints> hints = new osg::TessellationHints;
            hints->setDetailRatio(0.5f);
            osg::ref_ptr<osg::Geode> boxGeode = new osg::Geode;
            boxGeode->addDrawable(new osg::ShapeDrawable(
                new osg::Box(osg::Vec3(0.0f, 0.0f, 0.0f), 4.0f, 3.0f, 2.0f), hints.get()));

            std::vector<osg::Vec3> boxVerts; std::vector<unsigned int> boxTris;
            collectSoup(*boxGeode, boxVerts, boxTris);
            osg::ref_ptr<osg::Geometry> boxGeom = createSoupGeometry(boxVerts, boxTris);

            std::vector<osgVerse::PointList3D> lines;
            bool ok = osgVerse::extractFeatureLines(*boxGeom, 30.0f, lines);
            size_t numPoints = 0;
            for (size_t i = 0; i < lines.size(); ++i) numPoints += lines[i].size();
            std::cout << "[FeatureLines] " << (ok ? "ok" : "failed") << ", " << lines.size()
                      << " line(s), " << numPoints << " point(s)" << std::endl;

            osg::ref_ptr<osg::Geometry> lineGeom =
                osgVerse::createFeatureLineGeometry(lines, osg::Vec4(1.0f, 0.9f, 0.1f, 1.0f));
            if (lineGeom.valid())
            {
                osg::ref_ptr<osg::Geode> lineGeode = new osg::Geode;
                lineGeode->addDrawable(lineGeom.get());
                addDemoNode(root.get(), lineGeode.get(),
                            osg::Matrix::scale(2.0f, 2.0f, 2.0f) *
                            osg::Matrix::translate(0.0f, 0.0f, 45.0f));
            }
        }
    }
    else
        std::cout << "[MeshFixer] cinolib is not found, skip the new modeling demos" << std::endl;

    /*osg::Image* image = new osg::Image;
    image->allocateImage(1024, 512, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    osg::Camera* camera = osgVerse::createRTTCamera(osg::Camera::COLOR_BUFFER0, image, scene);
    osgVerse::alignCameraToBox(camera, bvv.getBoundingBox(), 1024, 512);
    root->addChild(camera);*/

#if false  // Test HDBScan
    osg::BoundingBox bb0(osg::Vec3(), osg::Vec3(10.0f, 10.0f, 10.0f));
    osg::BoundingSphere bs1(osg::Vec3(20.0f, 20.0f, 10.0f), 8.0);
    std::vector<osg::Vec3d> srcPts;
    for (int i = 0; i < 200; ++i) srcPts.push_back(osgVerse::createRandomVector(bb0, false));
    for (int i = 0; i < 200; ++i) srcPts.push_back(osgVerse::createRandomVector(bs1, false));

    osgVerse::HDBScanCluster cluster(srcPts);
    unsigned int num = cluster.execute(5, 1, osgVerse::HDBScanCluster::Euclidean);
    
    std::vector<int> labels; cluster.getLabels(labels);  // num = 2, labelA (0-20), labelB (21-40)
    std::cout << "HDBSCAN " << num << ": LABELS = " << labels.size() << "\n";
    for (unsigned int i = 0; i < labels.size(); ++i) std::cout << labels[i] << "; "; std::cout << "\n";
#endif

    osgViewer::Viewer viewer;
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getStateSet()));
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.setSceneData(root.get());
    viewer.setUpViewOnSingleScreen(0);
    return viewer.run();
}
