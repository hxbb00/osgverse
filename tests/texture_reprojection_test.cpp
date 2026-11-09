#include <osg/io_utils>
#include <osg/ArgumentParser>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Timer>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <iostream>

#include <VerseCommon.h>
#include <modeling/MeshBoolean.h>
#include <pipeline/Utilities.h>
#include <readerwriter/TextureReprojector.h>
#include <readerwriter/Utilities.h>

#ifndef _DEBUG
#include <backward.hpp>  // for better debug info
namespace backward { backward::SignalHandling sh; }
#endif

static void printUsage()
{
    std::cout << "Usage: osgVerse_Test_Texture_Reprojection --model <file> [options]\n"
              << "  --model2 <file>         second operand for --boolean\n"
              << "  --boolean <op>          boolean the two models before re-texturing:\n"
              << "                          union | intersection | difference | xor\n"
              << "  --boolean-demo          self-contained demo: boolean of two textured boxes\n"
              << "  --out <file.osgb>       save the re-projected geometry, otherwise show it\n"
              << "  --atlas <file.png>      save the baked texture atlas\n"
              << "  --angle <deg>           orbit angular step (default 30)\n"
              << "  --vstart/--vrange <deg> elevation range (default -90 / 180)\n"
              << "  --size <W> | --width/--height <N>   captured image resolution (default 1024)\n"
              << "  --fov <deg>             capture camera field of view (default 60)\n"
              << "  --ratio <0-1>           simplify the mesh before re-projection (rough LOD)\n"
              << "  --max-error <e>         maximum simplification error (default 1.0)\n"
              << "  --no-lock-border        allow simplifying border vertices\n"
              << "  --max-texture <size>    maximum atlas dimension\n"
              << "  --smooth-views <n>      view-selection smoothing iterations\n"
              << "  --padding <n>           atlas patch padding (default 2)\n"
              << "  --inpaint <n>           atlas inpaint radius (default 5)\n"
              << "  --threads <n>           threads used by texture mapping\n"
              << "  --no-occlusion          disable occlusion testing in view selection\n"
              << "  --no-color-correction   disable seam color correction\n"
              << "  --dump-views <prefix>   save every captured orbit view as <prefix>N.png\n"
              << "  --keep-shaders          keep the model's shader programs while capturing\n"
              << "  --view                  show original and re-projected models side by side\n"
              << std::endl;
}

static bool parseBooleanOp(const std::string& name, osgVerse::MeshBoolean::Operation& op)
{
    std::string n = osgDB::convertToLowerCase(name);
    if (n == "union") { op = osgVerse::MeshBoolean::Union; return true; }
    if (n == "intersection" || n == "intersect") { op = osgVerse::MeshBoolean::Intersection; return true; }
    if (n == "difference" || n == "subtract" || n == "subtraction")
    { op = osgVerse::MeshBoolean::Difference; return true; }
    if (n == "xor") { op = osgVerse::MeshBoolean::Xor; return true; }
    return false;
}

static osg::Image* createCheckerImage(int size, const osg::Vec4& colorA, const osg::Vec4& colorB)
{
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    const int cell = osg::maximum(1, size / 8);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            image->setColor(((x / cell + y / cell) & 1) ? colorA : colorB, x, y);
    return image.release();
}

// A closed box with per-face UVs, used by --boolean-demo so the example needs no assets.
static osg::Geometry* createTexturedBox(const osg::Vec3& center, float halfSize)
{
    static const osg::Vec3 normals[6] = {
        osg::Vec3(0.0f, -1.0f, 0.0f), osg::Vec3(0.0f, 1.0f, 0.0f),
        osg::Vec3(1.0f, 0.0f, 0.0f), osg::Vec3(-1.0f, 0.0f, 0.0f),
        osg::Vec3(0.0f, 0.0f, 1.0f), osg::Vec3(0.0f, 0.0f, -1.0f)
    };
    static const int cornerSigns[6][4][3] = {
        { {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1} },   // -Y
        { {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}, {-1, 1, 1} },       // +Y
        { {1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1} },       // +X
        { {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}, {-1, -1, 1} },   // -X
        { {-1, 1, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1} },       // +Z
        { {1, 1, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1} }    // -Z
    };
    static const osg::Vec2 uvs[4] = {
        osg::Vec2(0.0f, 1.0f), osg::Vec2(0.0f, 0.0f),
        osg::Vec2(1.0f, 0.0f), osg::Vec2(1.0f, 1.0f)
    };

    osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec3Array> na = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec2Array> ta = new osg::Vec2Array;
    osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
    for (int f = 0; f < 6; ++f)
    {
        unsigned int base = (unsigned int)va->size();
        for (int c = 0; c < 4; ++c)
        {
            va->push_back(center + osg::Vec3((float)cornerSigns[f][c][0],
                                             (float)cornerSigns[f][c][1],
                                             (float)cornerSigns[f][c][2]) * halfSize);
            na->push_back(normals[f]);
            ta->push_back(uvs[c]);
        }
        de->push_back(base + 0); de->push_back(base + 1); de->push_back(base + 2);
        de->push_back(base + 0); de->push_back(base + 2); de->push_back(base + 3);
    }

    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
    geom->setVertexArray(va.get());
    geom->setNormalArray(na.get(), osg::Array::BIND_PER_VERTEX);
    geom->setTexCoordArray(0, ta.get());
    geom->addPrimitiveSet(de.get());
    return geom.release();
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osgVerse::updateOsgBinaryWrappers();

    // Standalone example: orbit a complex model with virtual cameras, re-project the rendered
    // views onto its (optionally simplified) geometry with TextureMapping, and produce a new
    // geometry plus one texture atlas.
    // e.g. --model /path/to/model.osgb --out result.osgb --angle 30 --ratio 0.5
    std::string inputFile, outputFile, atlasFile, viewDumpPrefix, inputFile2, booleanName;
    arguments.read("--model", inputFile);
    if (inputFile.empty() && arguments.argc() > 1 && arguments[1][0] != '-')
        inputFile = arguments[1];
    arguments.read("--model2", inputFile2);
    arguments.read("--boolean", booleanName);
    bool booleanDemo = arguments.read("--boolean-demo");
    arguments.read("--out", outputFile);
    arguments.read("--atlas", atlasFile);
    arguments.read("--dump-views", viewDumpPrefix);
    if (inputFile.empty() && !booleanDemo)
    {
        printUsage();
        return 1;
    }
    if (!booleanName.empty() && inputFile2.empty() && !booleanDemo)
    {
        std::cout << "--boolean requires --model2 <file> (or use --boolean-demo)" << std::endl;
        return 1;
    }

    float angleStep = 30.0f, vStart = -90.0f, vRange = 180.0f, fov = 60.0f;
    int width = 1024, height = 1024, maxTexture = 0;
    float ratio = 0.0f, maxError = 1.0f;
    arguments.read("--angle", angleStep);
    arguments.read("--vstart", vStart);
    arguments.read("--vrange", vRange);
    arguments.read("--fov", fov);
    arguments.read("--size", width); height = width;
    arguments.read("--width", width);
    arguments.read("--height", height);
    arguments.read("--ratio", ratio);
    arguments.read("--max-error", maxError);
    arguments.read("--max-texture", maxTexture);

    // The atlas must be written to a known place when the result is saved as .osgb, as the
    // texture image is referenced by its file name
    if (atlasFile.empty() && !outputFile.empty())
        atlasFile = osgDB::getNameLessExtension(outputFile) + "_tex.png";

    osg::ref_ptr<osg::Node> model, model2;
    if (booleanDemo)
    {
        osg::ref_ptr<osg::Image> checker = createCheckerImage(
            128, osg::Vec4(1.0f, 0.55f, 0.2f, 1.0f), osg::Vec4(0.2f, 0.5f, 1.0f, 1.0f));
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(checker.get());
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

        osg::ref_ptr<osg::Geode> boxA = new osg::Geode;
        boxA->addDrawable(createTexturedBox(osg::Vec3(0.0f, 0.0f, 0.0f), 1.0f));
        osgVerse::applyTextureAttributeAndModes(
            boxA->getOrCreateStateSet(), 0, texture.get());
        osg::ref_ptr<osg::Geode> boxB = new osg::Geode;
        boxB->addDrawable(createTexturedBox(osg::Vec3(0.8f, 0.0f, 0.0f), 1.0f));
        osgVerse::applyTextureAttributeAndModes(
            boxB->getOrCreateStateSet(), 0, texture.get());

        model = boxA.get(); model2 = boxB.get();
        booleanName = "difference";
        inputFile = "textured box A"; inputFile2 = "textured box B";
    }
    else
    {
        model = osgDB::readNodeFile(inputFile);
        if (!model.valid())
        {
            std::cout << "Failed to load model: " << inputFile << std::endl;
            return 1;
        }
    }

    // Optional boolean stage: create the result geometry, then keep the two original models
    // (with their textures) as the capture source for re-texturing that result.
    osg::ref_ptr<osg::Node> displayModel = model;
    osg::ref_ptr<osg::Geometry> booleanResult;
    if (!booleanName.empty())
    {
        osgVerse::MeshBoolean::Operation op = osgVerse::MeshBoolean::Difference;
        if (!parseBooleanOp(booleanName, op))
        {
            std::cout << "Unknown boolean operation: " << booleanName << std::endl;
            return 1;
        }

        osg::ref_ptr<osg::Node> model2Node = model2;
        if (!model2Node.valid())
        {
            model2Node = osgDB::readNodeFile(inputFile2);
            if (!model2Node.valid())
            {
                std::cout << "Failed to load second model: " << inputFile2 << std::endl;
                return 1;
            }
        }

        // Move the second operand so that the two bounding spheres overlap; the same
        // transforms are used for the boolean and for the capture source group.
        osg::ComputeBoundsVisitor cbvA, cbvB;
        model->accept(cbvA); model2Node->accept(cbvB);
        osg::BoundingBox bbA = cbvA.getBoundingBox(), bbB = cbvB.getBoundingBox();
        float overlap = 0.5f * (bbA.radius() + bbB.radius());
        osg::Matrix matrixB = osg::Matrix::translate(
            bbA.center() - bbB.center() + osg::Vec3(overlap, 0.0f, 0.0f));

        osg::ref_ptr<osg::MatrixTransform> mtA = new osg::MatrixTransform;
        mtA->addChild(model.get());
        osg::ref_ptr<osg::MatrixTransform> mtB = new osg::MatrixTransform;
        mtB->setMatrix(matrixB); mtB->addChild(model2Node.get());

        osg::ref_ptr<osg::Group> source = new osg::Group;
        source->addChild(mtA.get()); source->addChild(mtB.get());

        osgVerse::MeshBoolean boolean(op);
        booleanResult = boolean.compute(mtA.get(), mtB.get());
        if (!booleanResult.valid())
        {
            std::cout << "Boolean operation failed: " << boolean.getLastError() << std::endl;
            return 1;
        }
        displayModel = source.get();
        std::cout << "Boolean source: " << inputFile << " + " << inputFile2 << std::endl;
    }

    osg::Timer timer;
    osgVerse::TextureReprojector reprojector(angleStep, width);
    reprojector.setResolution(width, height);
    reprojector.setVerticalRange(vStart, vRange);
    reprojector.setCameraFov(fov);
    reprojector.setSimplifyRatio(ratio);
    reprojector.setSimplifyMaxError(maxError);
    reprojector.setSimplifyLockBorder(!arguments.read("--no-lock-border"));
    reprojector.setMaxTextureSize(maxTexture);
    reprojector.setStripShaderPrograms(!arguments.read("--keep-shaders"));
    reprojector.setAtlasFile(atlasFile);
    reprojector.setViewDumpPrefix(viewDumpPrefix);

    osgVerse::TextureMapping::MeshTextureMappingOptions& mo = reprojector.getMappingOptions();
    arguments.read("--smooth-views", mo.view_selection_smoothing_iterations);
    arguments.read("--padding", mo.atlas_patch_padding);
    arguments.read("--inpaint", mo.inpaint_radius);
    arguments.read("--threads", mo.num_threads);
    if (arguments.read("--no-occlusion")) mo.enable_occlusion_testing = false;
    if (arguments.read("--no-color-correction")) mo.apply_color_correction = false;

    osg::ref_ptr<osg::Geometry> baked =
        booleanResult.valid() ? reprojector.reproject(displayModel.get(), booleanResult.get())
                              : reprojector.reproject(displayModel.get());
    if (!baked.valid())
    {
        std::cout << "Re-projection failed." << std::endl;
        return 1;
    }

    const osgVerse::TextureMapping::MeshTextureMappingResult& result = reprojector.getResult();
    std::cout << "Re-projected in " << timer.time_m() << "ms with " << reprojector.getNumViews()
              << " orbit views: " << result.face_uvs.size() / 6 << " faces, atlas "
              << result.atlas_width << "x" << result.atlas_height << std::endl;

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(baked.get());
    if (!outputFile.empty())
    {
        if (osgDB::writeNodeFile(*geode, outputFile))
            std::cout << "Saved to " << outputFile << " (texture: " << atlasFile << ")" << std::endl;
        else
            std::cout << "Failed to save " << outputFile << std::endl;
    }

    if (outputFile.empty() || arguments.read("--view"))
    {
        // Show the original model and the re-projected result side by side
        osg::BoundingSphere bs = displayModel->getBound();
        float offset = (bs.radius() > 0.0f ? bs.radius() : 1.0f) * 1.2f;
        osg::ref_ptr<osg::MatrixTransform> left = new osg::MatrixTransform;
        left->setMatrix(osg::Matrix::translate(-offset, 0.0f, 0.0f));
        left->addChild(displayModel.get());

        osg::ref_ptr<osg::MatrixTransform> right = new osg::MatrixTransform;
        right->setMatrix(osg::Matrix::translate(offset, 0.0f, 0.0f));
        right->addChild(geode.get());

        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->addChild(left.get()); root->addChild(right.get());

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
        // GLCore/GLES have no fixed-function pipeline: use an unlit "texture0 * vertex color"
        // program, with a white default texture for untextured models
        osg::StateSet* ss = root->getOrCreateStateSet();
        ss->setAttribute(osgVerse::createDefaultProgram("baseTexture"));
        ss->addUniform(new osg::Uniform("baseTexture", (int)0));
        osgVerse::applyTextureAttributeAndModes(
            ss, 0, osgVerse::createDefaultTexture(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f)));
#endif

        osgViewer::Viewer viewer;
        viewer.addEventHandler(new osgViewer::StatsHandler);
        viewer.addEventHandler(new osgViewer::WindowSizeHandler);
        viewer.setCameraManipulator(new osgGA::TrackballManipulator);
        viewer.setSceneData(root.get());
        return viewer.run();
    }
    return 0;
}
