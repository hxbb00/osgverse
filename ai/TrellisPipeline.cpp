extern "C"
{
    #include <trellis2/trellis2_capi.h>
}
#include <mutex>
#include "TrellisPipeline.h"

using namespace osgVerse;
#define TP_LOG(m) { OSG_WARN << "[Trellis2] " << m << std::endl; return; }
#define TP_LOG_R(m, r) { OSG_WARN << "[Trellis2] " << m << std::endl; return r; }

namespace
{
    // Thread-local callbacks for C interop (t2_generate is NOT thread-safe per pipeline)
    thread_local Trellis2::ProgressCallback g_progressCb = nullptr;
    thread_local Trellis2::PreviewCallback g_previewCb = nullptr;

    extern "C" void progressCallback(void* user, int stage, int step, int total)
    { if (g_progressCb) g_progressCb(stage, step, total); }

    extern "C" void previewCallback(void* user, int stage, int step, int total,
                                    const void* data, int len)
    {
        if (g_previewCb)
        {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            g_previewCb(stage, step, total, std::vector<unsigned char>(bytes, bytes + len));
        }
    }

    // Helper: convert C mesh result -> OSG MeshResult (deep copy)
    osg::ref_ptr<Trellis2::MeshResult> convertMesh(t2_mesh_result* r)
    {
        if (!r) return nullptr;
        int nv = t2_mesh_n_verts(r);
        int nt = t2_mesh_n_tris(r);
        if (nv == 0 || nt == 0)
        {
            t2_mesh_free(r);
            return nullptr;
        }

        osg::ref_ptr<Trellis2::MeshResult> mesh = new Trellis2::MeshResult;
        mesh->vertices = new osg::Vec3Array(nv);
        mesh->normals  = new osg::Vec3Array(nv);
        mesh->triangles = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, nt * 3);

        const float* verts = t2_mesh_verts(r);
        const float* norms = t2_mesh_normals(r);
        const int*   tris  = t2_mesh_tris(r);
        for (int i = 0; i < nv; ++i)
        {
            (*mesh->vertices)[i].set(verts[i*3], verts[i*3+1], verts[i*3+2]);
            (*mesh->normals)[i].set(norms[i*3], norms[i*3+1], norms[i*3+2]);
        }

        for (int i = 0; i < nt * 3; ++i)
        { (*mesh->triangles)[i] = static_cast<unsigned int>(tris[i]); }

        if (t2_mesh_has_pbr(r))
        {
            const float* pbr = t2_mesh_pbr(r);
            mesh->pbr.assign(pbr, pbr + 6 * nv);
        }
        t2_mesh_free(r);
        return mesh;
    }

    // Minimal BMP encoder so osg::Image can be fed to stb_image inside t2_generate
    static std::vector<unsigned char> encodeBMP(const osg::Image& img)
    {
        int w = img.s(), h = img.t();
        int rowSize = ((w * 3 + 3) / 4) * 4;
        int dataSize = rowSize * h;
        int fileSize = 54 + dataSize;

        std::vector<unsigned char> out(fileSize, 0);
        auto writeLE32 = [&](int offset, int value)
        {
            out[offset]   = static_cast<unsigned char>(value & 0xFF);
            out[offset+1] = static_cast<unsigned char>((value >> 8) & 0xFF);
            out[offset+2] = static_cast<unsigned char>((value >> 16) & 0xFF);
            out[offset+3] = static_cast<unsigned char>((value >> 24) & 0xFF);
        };

        out[0] = 'B'; out[1] = 'M';
        writeLE32(2, fileSize);
        writeLE32(10, 54); writeLE32(14, 40);
        writeLE32(18, w); writeLE32(22, h);
        out[26] = 1; out[28] = 24;
        writeLE32(34, dataSize);

        int srcChannels = img.getPixelSizeInBits() / 8;
        const unsigned char* src = img.data();
        for (int y = 0; y < h; ++y)
        {
            int dstRow = 54 + (h - 1 - y) * rowSize;
            for (int x = 0; x < w; ++x)
            {
                int srcIdx = (y * w + x) * srcChannels;
                int dstIdx = dstRow + x * 3;
                out[dstIdx + 0] = (srcChannels >= 3) ? src[srcIdx + 2] : src[srcIdx]; // B
                out[dstIdx + 1] = (srcChannels >= 3) ? src[srcIdx + 1] : src[srcIdx]; // G
                out[dstIdx + 2] = (srcChannels >= 3) ? src[srcIdx + 0] : src[srcIdx]; // R
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------------
    // Trellis2Impl
    // ---------------------------------------------------------------------------
    class Trellis2Impl : public osg::Referenced
    {
    public:
        t2_pipeline* pipeline = nullptr;
        std::string dino, ssFlow, ssDec, slatFlow, slatHRFlow, shapeDec;
        std::string shapeEnc, texDec, texFlow, texFlowHR;
        int loadFlags = 0;
        mutable std::mutex mutex;

        Trellis2Impl(const std::string& dinoGGUF, const std::string& ssFlowGGUF,
                     const std::string& ssDecGGUF, const std::string& slatFlowGGUF,
                     const std::string& slatHRFlowGGUF, const std::string& shapeDecGGUF,
                     const std::string& shapeEncGGUF, const std::string& texDecGGUF,
                     const std::string& texFlowGGUF, const std::string& texFlowHRGGUF,
                     int flags)
        {
            dino = dinoGGUF; ssFlow = ssFlowGGUF; ssDec = ssDecGGUF;
            slatFlow = slatFlowGGUF; slatHRFlow = slatHRFlowGGUF; shapeDec = shapeDecGGUF;
            shapeEnc = shapeEncGGUF; texDec = texDecGGUF;
            texFlow = texFlowGGUF; texFlowHR = texFlowHRGGUF;
            loadFlags = flags; load();
        }

        ~Trellis2Impl()
        { if (pipeline) t2_pipeline_free(pipeline); }

        bool load()
        {
            if (pipeline) return true;
            char err[512] = {0};
            pipeline = t2_pipeline_load(
                dino.empty()     ? nullptr : dino.c_str(),
                ssFlow.empty()   ? nullptr : ssFlow.c_str(),
                ssDec.empty()    ? nullptr : ssDec.c_str(),
                slatFlow.empty() ? nullptr : slatFlow.c_str(),
                slatHRFlow.empty()? nullptr : slatHRFlow.c_str(),
                shapeDec.empty() ? nullptr : shapeDec.c_str(),
                shapeEnc.empty() ? nullptr : shapeEnc.c_str(),
                texDec.empty()   ? nullptr : texDec.c_str(),
                texFlow.empty()  ? nullptr : texFlow.c_str(),
                texFlowHR.empty()? nullptr : texFlowHR.c_str(),
                loadFlags, err, sizeof(err));

            if (!pipeline)
            {
                OSG_WARN << "[Trellis2] Pipeline load failed: " << err << std::endl;
                return false;
            }
            return true;
        }

        void unload()
        {
            if (pipeline)
            {
                t2_pipeline_free(pipeline);
                pipeline = nullptr;
            }
        }

        bool isLoaded() const { return pipeline != nullptr; }
        int getCaps() const { if (!pipeline) return 0; return t2_pipeline_caps(pipeline); }
        std::string getBackend() const { return (!pipeline) ? "" : t2_pipeline_backend(pipeline); }
        
        int getConfiguredCaps() const
        {
            // Mirrors engine.go configuredCaps: infer from model paths when unloaded
            int caps = Trellis2::CAP_COARSE;
            if (!slatFlow.empty() && !shapeDec.empty())
            {
                caps |= Trellis2::CAP_512;
                if (!slatHRFlow.empty()) caps |= Trellis2::CAP_1024;
                if (!shapeEnc.empty() && !texDec.empty() && !texFlow.empty())
                    caps |= Trellis2::CAP_TEXTURE;
            }
            return caps;
        }
    };
}

// ---------------------------------------------------------------------------
// Trellis2
// ---------------------------------------------------------------------------
Trellis2::Trellis2(const std::string& dinoGGUF, const std::string& ssFlowGGUF,
                   const std::string& ssDecGGUF, const std::string& slatFlowGGUF,
                   const std::string& slatHRFlowGGUF, const std::string& shapeDecGGUF,
                   const std::string& shapeEncGGUF, const std::string& texDecGGUF,
                   const std::string& texFlowGGUF, const std::string& texFlowHRGGUF,
                   int loadFlags)
{
    _internal = new Trellis2Impl(dinoGGUF, ssFlowGGUF, ssDecGGUF, slatFlowGGUF,
                                 slatHRFlowGGUF, shapeDecGGUF, shapeEncGGUF,
                                 texDecGGUF, texFlowGGUF, texFlowHRGGUF, loadFlags);
}

Trellis2::~Trellis2()
{}

bool Trellis2::checkModelInformation(ModelInformation& info)
{
    Trellis2Impl* p = static_cast<Trellis2Impl*>(_internal.get());
    if (p->isLoaded())
    {
        info.backend = p->getBackend();
        info.caps = p->getCaps();
    }
    else
    {
        info.backend = "GPU (models unloaded)";
        info.caps = p->getConfiguredCaps();
    }
    info.textured = (info.caps & CAP_TEXTURE) != 0;
    return true;
}

bool Trellis2::isLoaded() const
{
    Trellis2Impl* p = static_cast<Trellis2Impl*>(_internal.get());
    return p->isLoaded();
}

void Trellis2::unload()
{
    Trellis2Impl* p = static_cast<Trellis2Impl*>(_internal.get());
    std::lock_guard<std::mutex> lock(p->mutex);
    p->unload();
}

bool Trellis2::reload()
{
    Trellis2Impl* p = static_cast<Trellis2Impl*>(_internal.get());
    std::lock_guard<std::mutex> lock(p->mutex);
    return p->load();
}

// ---------------------------------------------------------------------------
// Generate (raw bytes)
// ---------------------------------------------------------------------------
osg::ref_ptr<Trellis2::MeshResult> Trellis2::generate(
        const std::vector<unsigned char>& imageBytes, PipelineType pipelineType,
        BackgroundMode backgroundMode, uint64_t seed, int steps, float guidance,
        int textureSteps, ProgressCallback onProgress, PreviewCallback onPreview)
{
    Trellis2Impl* p = static_cast<Trellis2Impl*>(_internal.get());
    std::lock_guard<std::mutex> lock(p->mutex);
    if (!p->isLoaded() && !p->load()) TP_LOG_R("Pipeline not loaded", NULL);

    char err[512] = {0};
    g_progressCb = onProgress;
    g_previewCb  = onPreview;
    t2_mesh_result* r = t2_generate(
        p->pipeline,
        imageBytes.empty() ? nullptr : imageBytes.data(),
        static_cast<int>(imageBytes.size()),
        static_cast<int>(pipelineType),
        static_cast<int>(backgroundMode),
        seed, steps, guidance, textureSteps,
        onProgress ? progressCallback : nullptr, nullptr,
        onPreview  ? previewCallback  : nullptr, nullptr,
        err, sizeof(err));
    
    g_progressCb = nullptr;
    g_previewCb  = nullptr;
    if (!r) TP_LOG_R(std::string("Generate failed: ") + err, NULL);
    return convertMesh(r);
}

// ---------------------------------------------------------------------------
// Generate (osg::Image)
// ---------------------------------------------------------------------------
osg::ref_ptr<Trellis2::MeshResult> Trellis2::generate(
        const osg::Image& image, PipelineType pipelineType,
        BackgroundMode backgroundMode, uint64_t seed,
        int steps, float guidance, int textureSteps,
        ProgressCallback onProgress, PreviewCallback onPreview)
{
    std::vector<unsigned char> encoded = encodeBMP(image);
    return generate(encoded, pipelineType, backgroundMode, seed, steps, guidance,
                    textureSteps, onProgress, onPreview);
}

// ---------------------------------------------------------------------------
// Prepare mesh
// ---------------------------------------------------------------------------
osg::ref_ptr<Trellis2::MeshResult> Trellis2::prepareMesh(const MeshResult* mesh, int componentFilter)
{
    if (!mesh || !mesh->vertices || mesh->vertices->empty() ||
        !mesh->triangles || mesh->triangles->empty())
    { TP_LOG_R("empty mesh", NULL); }

    int nv = static_cast<int>(mesh->vertices->size());
    int nt = static_cast<int>(mesh->triangles->size()) / 3;
    std::vector<float> verts(nv * 3); std::vector<int> tris(nt * 3);
    for (int i = 0; i < nv; ++i)
    {
        verts[i*3]   = (*mesh->vertices)[i].x();
        verts[i*3+1] = (*mesh->vertices)[i].y();
        verts[i*3+2] = (*mesh->vertices)[i].z();
    }
    for (int i = 0; i < nt * 3; ++i)
    { tris[i] = static_cast<int>((*mesh->triangles)[i]); }

    const float* pbrPtr = nullptr; std::vector<float> pbrData;
    if (mesh->hasPBR()) { pbrData = mesh->pbr; pbrPtr = pbrData.data(); }

    char err[512] = {0};
    t2_mesh_result* r = t2_prepare_mesh(verts.data(), nv, tris.data(), nt,
                                        pbrPtr, componentFilter, err, sizeof(err));
    if (!r) TP_LOG_R(std::string("PrepareMesh failed: ") + err, NULL);
    return convertMesh(r);
}

// ---------------------------------------------------------------------------
// Print remesh
// ---------------------------------------------------------------------------
bool Trellis2::hasPrintRemesh() const
{ return t2_print_remesh_available() != 0; }

osg::ref_ptr<Trellis2::MeshResult> Trellis2::preparePrintMesh(const MeshResult* mesh, int componentFilter,
                                                              float alphaRatio, float offsetRatio)
{
    if (!hasPrintRemesh()) TP_LOG_R("Print remeshing unavailable (library built without CGAL)", NULL);
    if (!mesh || !mesh->vertices || mesh->vertices->empty()) TP_LOG_R("Empty mesh", NULL);

    int nv = static_cast<int>(mesh->vertices->size());
    int nt = static_cast<int>(mesh->triangles->size()) / 3;
    std::vector<float> verts(nv * 3); std::vector<int> tris(nt * 3);
    for (int i = 0; i < nv; ++i)
    {
        verts[i*3]   = (*mesh->vertices)[i].x();
        verts[i*3+1] = (*mesh->vertices)[i].y();
        verts[i*3+2] = (*mesh->vertices)[i].z();
    }
    for (int i = 0; i < nt * 3; ++i)
    { tris[i] = static_cast<int>((*mesh->triangles)[i]); }

    const float* pbrPtr = nullptr; std::vector<float> pbrData;
    if (mesh->hasPBR()) { pbrData = mesh->pbr; pbrPtr = pbrData.data(); }

    char err[512] = {0};
    t2_mesh_result* r = t2_prepare_print_mesh(verts.data(), nv, tris.data(), nt, pbrPtr,
                                              componentFilter, alphaRatio, offsetRatio, err, sizeof(err));
    if (!r) TP_LOG_R(std::string("preparePrintMesh failed: ") + err, NULL);
    return convertMesh(r);
}

// ---------------------------------------------------------------------------
// Bake GLB
// ---------------------------------------------------------------------------
std::vector<unsigned char> Trellis2::bakeGLB(const MeshResult* mesh,
                                             int textureSize, int componentFilter)
{
    if (!mesh || !mesh->vertices || mesh->vertices->empty()) TP_LOG_R("Empty mesh", {});
    int nv = static_cast<int>(mesh->vertices->size());
    int nt = static_cast<int>(mesh->triangles->size()) / 3;

    std::vector<float> verts(nv * 3); std::vector<int> tris(nt * 3);
    for (int i = 0; i < nv; ++i)
    {
        verts[i*3]   = (*mesh->vertices)[i].x();
        verts[i*3+1] = (*mesh->vertices)[i].y();
        verts[i*3+2] = (*mesh->vertices)[i].z();
    }
    for (int i = 0; i < nt * 3; ++i)
    { tris[i] = static_cast<int>((*mesh->triangles)[i]); }

    const float* pbrPtr = nullptr; std::vector<float> pbrData;
    if (mesh->hasPBR()) { pbrData = mesh->pbr; pbrPtr = pbrData.data(); }

    int outLen = 0; char err[512] = {0};
    uint8_t* buf = t2_bake_glb(verts.data(), nv, tris.data(), nt, pbrPtr,
                               textureSize, componentFilter, &outLen, err, sizeof(err));
    if (!buf) TP_LOG_R(std::string("Bake GLB failed: ") + err, {});

    std::vector<unsigned char> result(buf, buf + outLen);
    t2_free_buffer(buf); return result;
}

// ---------------------------------------------------------------------------
// Preprocess image
// ---------------------------------------------------------------------------
bool Trellis2::preprocessImage(const std::vector<unsigned char>& imageBytes,
                               int outSize, std::vector<unsigned char>& outRGB)
{
    char err[512] = {0}; outRGB.resize(outSize * outSize * 3);
    int rc = t2_preprocess_image_bytes(imageBytes.data(), static_cast<int>(imageBytes.size()),
                                       outSize, outRGB.data(), err, sizeof(err));
    if (rc != 0)
    {
        OSG_WARN << "[Trellis2] Image preprocess failed: " << err << std::endl;
        return false;
    }
    return true;
}
