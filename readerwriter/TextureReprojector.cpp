#include <osg/io_utils>
#include <osg/Geode>
#include <osg/Group>
#include <osgViewer/Viewer>
#include <osgUtil/SmoothingVisitor>
#include <osgDB/FileNameUtils>
#include <osgDB/WriteFile>
#include <meshoptimizer/meshoptimizer.h>
#include <nanoid/nanoid.h>
#include <cstring>
#include <sstream>
#include "modeling/Utilities.h"
#include "pipeline/Pipeline.h"
#include "pipeline/Utilities.h"
#include "TextureReprojector.h"

using namespace osgVerse;

namespace
{
    // Intrinsics matching the OSG perspective camera used for rendering, in the pinhole form
    // TextureMapping expects; pixel v is measured from the image bottom (osg::Image row 0),
    // and the depth is -Z in eye space.
    // NOTE: square pixels give f = height / (2 * tan(fovy / 2)), independent of the width
    osg::Matrixf computeIntrinsics(int w, int h, double fovyDeg)
    {
        double t = tan(osg::DegreesToRadians(fovyDeg) * 0.5);
        osg::Matrixf K;
        K(0, 0) = (float)(0.5 * h / t); K(0, 2) = -0.5f * w;
        K(1, 1) = (float)(0.5 * h / t); K(1, 2) = -0.5f * h;
        K(2, 2) = -1.0f;
        return K;
    }

    // OSG matrices use the row-vector convention while TextureMapping expects a column-vector
    // [R | t]: R is the transpose of the view rotation and t is the view's fourth row
    void decomposeView(const osg::Matrix& view, osg::Matrixf& R, osg::Vec3f& T)
    {
        R.set((float)view(0, 0), (float)view(1, 0), (float)view(2, 0), 0.0f,
              (float)view(0, 1), (float)view(1, 1), (float)view(2, 1), 0.0f,
              (float)view(0, 2), (float)view(1, 2), (float)view(2, 2), 0.0f,
              0.0f, 0.0f, 0.0f, 1.0f);
        T.set((float)view(3, 0), (float)view(3, 1), (float)view(3, 2));
    }

    // Unlit program used to capture the original appearance of shader-less models. GLCore and
    // GLES have no fixed-function pipeline, so geometry without a program would render black.
    // The captured color is texture0 * vertex color, which is the albedo of most photogrammetry
    // and merged-tile models
    osg::Program* createCaptureProgram()
    {
        const char* vsCode = {
            "VERSE_VS_OUT vec2 uv;\n"
            "VERSE_VS_OUT vec4 color;\n"
            "void main() {\n"
            "    uv = osg_MultiTexCoord0.st;\n"
            "    color = osg_Color;\n"
            "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
            "}\n"
        };
        const char* fsCode = {
            "uniform sampler2D BaseTexture;\n"
            "VERSE_FS_IN vec2 uv;\n"
            "VERSE_FS_IN vec4 color;\n"
            "VERSE_FS_OUT vec4 fragData;\n"
            "void main() {\n"
            "    fragData = color * VERSE_TEX2D(BaseTexture, uv);\n"
            "    VERSE_FS_FINAL(fragData);\n"
            "}\n"
        };

        int cxtVer = 0, glslVer = 0; guessOpenGLVersions(cxtVer, glslVer);
        osg::ref_ptr<osg::Shader> vs = new osg::Shader(osg::Shader::VERTEX, vsCode);
        osg::ref_ptr<osg::Shader> fs = new osg::Shader(osg::Shader::FRAGMENT, fsCode);
        Pipeline::createShaderDefinitions(vs.get(), cxtVer, glslVer);
        Pipeline::createShaderDefinitions(fs.get(), cxtVer, glslVer);

        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(vs.get()); program->addShader(fs.get());
        return program.release();
    }

    // Temporarily removes shader programs from a model, so the unlit capture program is used.
    // Geometries without a color array would read an undefined osg_Color attribute in GLCore
    // (the default value is not white, which tinted all captures); a white color array is
    // added to them. Everything is restored once the orbit views have been rendered
    struct CaptureModelVisitor : public osg::NodeVisitor
    {
        CaptureModelVisitor() : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}

        void strip(osg::StateSet* ss)
        {
            if (ss == NULL) return;
            osg::StateAttribute* prog = ss->getAttribute(osg::StateAttribute::PROGRAM);
            if (prog == NULL) return;
            _saved.push_back(std::make_pair(osg::ref_ptr<osg::StateSet>(ss),
                                            osg::ref_ptr<osg::StateAttribute>(prog)));
            ss->removeAttribute(osg::StateAttribute::PROGRAM);
        }

        void prepare(osg::Geometry* geom)
        {
            if (geom == NULL) return;
            strip(geom->getStateSet());
            if (geom->getColorArray() != NULL) return;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->assign(geom->getVertexArray() ? geom->getVertexArray()->getNumElements() : 0,
                           osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
            _colorGeometries.push_back(geom);
            geom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        }

        virtual void apply(osg::Node& node)
        { strip(node.getStateSet()); traverse(node); }

        virtual void apply(osg::Geode& geode)
        {
            strip(geode.getStateSet());
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                prepare(geode.getDrawable(i)->asGeometry());
            traverse(geode);
        }

        void restore()
        {
            for (size_t i = 0; i < _saved.size(); ++i)
                _saved[i].first->setAttributeAndModes(_saved[i].second.get());
            _saved.clear();
            for (size_t i = 0; i < _colorGeometries.size(); ++i)
                _colorGeometries[i]->setColorArray(NULL);
            _colorGeometries.clear();
        }

        std::vector<std::pair<osg::ref_ptr<osg::StateSet>, osg::ref_ptr<osg::StateAttribute>>> _saved;
        std::vector<osg::ref_ptr<osg::Geometry>> _colorGeometries;
    };

    // Offscreen renderer which captures the model from arbitrary view/projection matrices
    struct ViewRenderer
    {
        ~ViewRenderer()
        { if (_camera.valid() && _model != NULL) _camera->removeChild(_model); }

        bool setup(osg::Node* node, int w, int h)
        {
            _width = w; _height = h; _model = node;
            _viewer = new osgViewer::Viewer;
            _viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->x = 0; traits->y = 0; traits->width = w; traits->height = h;
            traits->windowDecoration = false; traits->doubleBuffer = false; traits->pbuffer = true;
            traits->readDISPLAY();
            osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
            if (!gc.valid() || !gc->valid())
            {
                OSG_WARN << "[TextureReprojector] Failed to create the offscreen graphics context"
                         << std::endl;
                return false;
            }
            _viewer->getCamera()->setGraphicsContext(gc.get());
            _viewer->getCamera()->setViewport(0, 0, w, h);

            _camera = new osg::Camera;
            // Clear to TRANSPARENT black: pixels not covered by the model keep alpha 0, so
            // baking can detect the background and leave it to the atlas inpainting instead
            // of baking black into faces which stick out of the original silhouette
            _camera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
            _camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            _camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
            _camera->setRenderOrder(osg::Camera::PRE_RENDER);
            _camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            _camera->setViewport(0, 0, w, h);

            // Attach ONE persistent readback image: swapping the attached image per view
            // breaks the FBO readback after the first render, so capture() copies it instead
            _readback = new osg::Image;
            _readback->allocateImage(w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            _readback->setInternalTextureFormat(GL_RGBA8);
            _camera->attach(osg::Camera::COLOR_BUFFER0, _readback.get());

            // A depth attachment is required: without it the depth test is a no-op and
            // overlapping surfaces render in submission order
            _camera->attach(osg::Camera::DEPTH_BUFFER, GL_DEPTH_COMPONENT24);

            // Bake the original albedo: lighting applied here would be baked into the atlas.
            // The capture program renders "vertexColor * texture0" unlit
            osg::StateSet* ss = _camera->getOrCreateStateSet();
            ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            applyTextureAttributeAndModes(ss, 0, createDefaultTexture(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f)));
            ss->addUniform(new osg::Uniform("BaseTexture", (int)0));
            ss->setAttributeAndModes(createCaptureProgram(), osg::StateAttribute::ON);

            _camera->addChild(node);
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(_camera.get());
            _viewer->setSceneData(root.get());
            return true;
        }

        osg::Image* capture(const osg::Matrix& view, const osg::Matrix& proj)
        {
            _camera->setViewMatrix(view);
            _camera->setProjectionMatrix(proj);
            for (int i = 0; i < 2; ++i) _viewer->frame();  // 2 frames: settle + readback

            osg::ref_ptr<osg::Image> copy = new osg::Image;
            copy->allocateImage(_width, _height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            copy->setInternalTextureFormat(GL_RGBA8);
            if (_readback->data())
                memcpy(copy->data(), _readback->data(), _readback->getTotalSizeInBytes());
            return copy.release();
        }

        osg::ref_ptr<osgViewer::Viewer> _viewer;
        osg::ref_ptr<osg::Camera> _camera;
        osg::ref_ptr<osg::Image> _readback;
        osg::Node* _model = NULL;
        int _width, _height;
    };
}

TextureReprojector::TextureReprojector(float angleStep, int resolution)
{
    _angleStep = angleStep; _horizontalStart = 0.0f; _horizontalRange = 360.0f;
    _verticalStart = -90.0f; _verticalRange = 180.0f;
    _fov = 60.0f; _distanceScale = 1.05f;
    _width = _height = resolution;
    _maxTextureSize = 0;
    _simplifyRatio = 0.0f; _simplifyMaxError = 1.0f; _simplifyLockBorder = true;
    _stripShaderPrograms = true;
}

TextureReprojector::~TextureReprojector()
{}

bool TextureReprojector::collectMesh(osg::Node* node, std::vector<osg::Vec3>& vertices,
                                     std::vector<unsigned int>& triangles)
{
    MeshCollector collector;
    collector.setWeldingVertices(true);
    collector.setOnlyVertexAndIndices(true);
    node->accept(collector);

    vertices = collector.getVertices();
    triangles = collector.getTriangles();
    if (vertices.empty() || triangles.empty())
    {
        OSG_WARN << "[TextureReprojector] The input model has no triangle data" << std::endl;
        return false;
    }
    return true;
}

bool TextureReprojector::simplifyMesh(std::vector<osg::Vec3>& vertices,
                                      std::vector<unsigned int>& triangles)
{
    if (_simplifyRatio <= 0.0f || _simplifyRatio >= 1.0f) return false;
    size_t targetCount = (size_t)((double)triangles.size() * _simplifyRatio / 3.0) * 3;
    if (targetCount < 3) targetCount = 3;

    std::vector<unsigned int> newTriangles(triangles.size());
    float resultError = 0.0f;
    size_t newCount = meshopt_simplify(
        newTriangles.data(), triangles.data(), triangles.size(),
        (const float*)vertices.data(), vertices.size(), sizeof(osg::Vec3),
        targetCount, _simplifyMaxError, _simplifyLockBorder ? meshopt_SimplifyLockBorder : 0,
        &resultError);
    newTriangles.resize(newCount);
    if (newTriangles.empty())
    {
        OSG_WARN << "[TextureReprojector] Simplification produced no triangle" << std::endl;
        return false;
    }

    // Compact the vertices which are still referenced by the simplified triangles
    std::vector<unsigned int> remap(vertices.size(), 0xFFFFFFFFu);
    unsigned int newVertexCount = 0;
    for (size_t i = 0; i < newTriangles.size(); ++i)
    {
        unsigned int& r = remap[newTriangles[i]];
        if (r == 0xFFFFFFFFu) r = newVertexCount++;
        newTriangles[i] = r;
    }
    std::vector<osg::Vec3> newVertices(newVertexCount);
    for (size_t i = 0; i < vertices.size(); ++i)
        if (remap[i] != 0xFFFFFFFFu) newVertices[remap[i]] = vertices[i];

    OSG_NOTICE << "[TextureReprojector] Simplified " << triangles.size() / 3 << " -> "
               << newTriangles.size() / 3 << " triangles (error " << resultError << ")"
               << std::endl;
    vertices.swap(newVertices); triangles.swap(newTriangles);
    return true;
}

osg::Geometry* TextureReprojector::reproject(osg::Node* node)
{
    _result = TextureMapping::MeshTextureMappingResult();
    _images.clear();
    _texture = NULL;
    if (node == NULL) return NULL;

    // 1) Collect the model as a world-space triangle soup, and simplify it if required.
    //    Simplification works on positions only, so it is not limited by the source UVs;
    //    the simplified mesh is re-textured from the original model in the next steps
    std::vector<osg::Vec3> vertices;
    std::vector<unsigned int> triangles;
    if (!collectMesh(node, vertices, triangles)) return NULL;
    simplifyMesh(vertices, triangles);

    // 2) Capture the ORIGINAL model from cameras orbiting its bounding sphere
    osg::BoundingBox bounds;
    for (size_t i = 0; i < vertices.size(); ++i) bounds.expandBy(vertices[i]);
    float radius = bounds.radius();
    if (radius < 1e-4f) radius = 1e-4f;
    osg::BoundingSphere bs(bounds.center(), radius);

    ViewRenderer renderer;
    CaptureModelVisitor captureVisitor;
    if (_stripShaderPrograms)
    {
        node->accept(captureVisitor);
        OSG_NOTICE << "[TextureReprojector] Removed " << captureVisitor._saved.size()
                   << " shader program(s) and prepared " << captureVisitor._colorGeometries.size()
                   << " uncolored geometry(ies) for capturing" << std::endl;
    }
    if (!renderer.setup(node, _width, _height))
    { captureVisitor.restore(); return NULL; }

    float aspect = (float)_width / (float)_height;
    std::vector<OrbitView> views = createOrbitViews(
        bs, _angleStep, _horizontalStart, _horizontalRange, _verticalStart, _verticalRange,
        _fov, aspect, _distanceScale);
    if (views.empty())
    {
        captureVisitor.restore();
        OSG_WARN << "[TextureReprojector] No orbit view can be generated" << std::endl;
        return NULL;
    }

    osg::Matrixf K = computeIntrinsics(_width, _height, _fov);
    for (size_t i = 0; i < views.size(); ++i)
    {
        osg::ref_ptr<osg::Image> image = renderer.capture(views[i].view, views[i].proj);
        if (!image.valid()) continue;

        if (!_viewDumpPrefix.empty())
        {
            std::stringstream ss; ss << _viewDumpPrefix << i << ".png";
            osgDB::writeImageFile(*image, ss.str());
        }

        // Skip views in which nothing is visible (e.g. the model renders black because its
        // shaders/textures are not supported by this offscreen context): such a view would
        // otherwise bake black patches into the atlas
        double sum = 0.0; int count = 0;
        for (int y = 0; y < _height; y += 16)
            for (int x = 0; x < _width; x += 16)
        { osg::Vec4 c = image->getColor(x, y); sum += c.a(); ++count; }
        if (count == 0 || sum / count < 1e-4)
        {
            OSG_WARN << "[TextureReprojector] View " << i << " is empty and is skipped" << std::endl;
            continue;
        }

        osg::Matrixf R; osg::Vec3f T;
        decomposeView(views[i].view, R, T);
        _images.push_back(TextureMapping::Image(K, R, T, TextureMapping::Bitmap(image.get())));
    }
    captureVisitor.restore();  // the original appearance of the input model is not modified
    if (_images.empty())
    {
        OSG_WARN << "[TextureReprojector] No usable orbit view was captured" << std::endl;
        return NULL;
    }

    // 3) Re-project the captured views onto the (simplified) mesh
    TextureMapping::Mesh mesh;
    mesh.vertices.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i)
        mesh.vertices.push_back(TextureMapping::Vertex::fromVec3(vertices[i]));
    mesh.faces.reserve(triangles.size() / 3);
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        mesh.faces.push_back(TextureMapping::Face(triangles[i], triangles[i + 1], triangles[i + 2]));

    _result = TextureMapping::process(mesh, _images, _options);
    if (_result.atlas_width == 0 || _result.face_uvs.size() != mesh.faces.size() * 6)
    {
        OSG_WARN << "[TextureReprojector] Texture mapping failed" << std::endl;
        return NULL;
    }

    size_t numEmptyFaces = 0;
    for (size_t i = 0; i < _result.face_baked_pixels.size(); ++i)
        if (_result.face_baked_pixels[i] == 0) ++numEmptyFaces;
    OSG_NOTICE << "[TextureReprojector] Re-projected " << mesh.faces.size() << " faces from "
               << _images.size() << " views, atlas " << _result.atlas_width << "x"
               << _result.atlas_height << ", " << numEmptyFaces
               << " faces have no baked pixel (filled by inpainting)" << std::endl;

    // 4) Bind the atlas and build the output geometry
    osg::ref_ptr<osg::Geometry> geometry = createGeometry(vertices, triangles);
    if (!geometry.valid()) return NULL;

    osg::ref_ptr<osg::Image> atlasImage = new osg::Image(*_result.texture_atlas.getImage());
    if (_maxTextureSize > 0 && atlasImage->s() > _maxTextureSize)
    {
        int newHeight = osg::maximum(1, atlasImage->t() * _maxTextureSize / atlasImage->s());
        atlasImage->scaleImage(_maxTextureSize, newHeight, 1);
    }
    if (!_atlasFile.empty()) osgDB::writeImageFile(*atlasImage, _atlasFile);
    {
        std::string name = osgDB::getSimpleFileName(_atlasFile);
        atlasImage->setFileName(name.empty() ? ("reproj_tex_" + nanoid::generate(8) + ".png") : name);
    }

    _texture = new osg::Texture2D(atlasImage.get());
    _texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    _texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    _texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    _texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    applyTextureAttributeAndModes(geometry->getOrCreateStateSet(), 0, _texture.get());
    return geometry.release();
}

osg::Geometry* TextureReprojector::createGeometry(const std::vector<osg::Vec3>& vertices,
                                                  const std::vector<unsigned int>& triangles)
{
    // Per-corner UVs: split a vertex only where the adjacent faces picked different views
    typedef std::pair<unsigned int, std::pair<int, int>> CornerKey;
    std::map<CornerKey, unsigned int> cornerMap;
    osg::ref_ptr<osg::Vec3Array> va = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec2Array> ua = new osg::Vec2Array;
    osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
    for (size_t fi = 0; fi * 3 + 2 < triangles.size(); ++fi)
    {
        for (int c = 0; c < 3; ++c)
        {
            unsigned int index = triangles[fi * 3 + c];
            // face_uvs is in the Colmap convention (v measured from the atlas top), while the
            // atlas image keeps osg::Image's bottom-up rows, so v is flipped back here
            float u = _result.face_uvs[fi * 6 + c * 2 + 0];
            float v = 1.0f - _result.face_uvs[fi * 6 + c * 2 + 1];
            CornerKey key(index, std::make_pair((int)floor(u * 65536.0f + 0.5f),
                                                (int)floor(v * 65536.0f + 0.5f)));
            std::map<CornerKey, unsigned int>::iterator itr = cornerMap.find(key);
            unsigned int outIndex;
            if (itr == cornerMap.end())
            {
                outIndex = (unsigned int)va->size();
                cornerMap[key] = outIndex;
                va->push_back(vertices[index]);
                ua->push_back(osg::Vec2(u, v));
            }
            else outIndex = itr->second;
            de->push_back(outIndex);
        }
    }

    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
    geom->setVertexArray(va.get());
    geom->setTexCoordArray(0, ua.get());
    geom->addPrimitiveSet(de.get());
    osgUtil::SmoothingVisitor::smooth(*geom);

    // Deferred pipelines shade with texture * osg_Color; without a color array the attribute
    // is undefined and the result renders black. Baked textures are unlit albedo, so fill
    // plain white per-vertex
    osg::ref_ptr<osg::Vec4Array> ca = new osg::Vec4Array;
    ca->assign(va->size(), osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
    geom->setColorArray(ca.get(), osg::Array::BIND_PER_VERTEX);
    geom->setName("ReprojectedGeometry");
    return geom.release();
}
