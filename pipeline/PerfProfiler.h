#ifndef OSGVERSE_PERF_PROFILER_H
#define OSGVERSE_PERF_PROFILER_H

#include <osg/Camera>
#include <osg/Node>
#include <osg/Referenced>
#include <osg/observer_ptr>
#include <map>
#include <ostream>
#include <string>
#include <vector>
#include "Global.h"

namespace osgVerse
{
    struct PerfProfilerImpl;

    /** PerfProfiler gathers per-frame rendering statistics for a camera (a whole pass) or for an
        arbitrary node / subgraph. It works with the standard osgVerse pipeline as well as with a
        plain forward osgViewer, because it only relies on core OSG extension points:

          - CAMERA scope: the camera's pre/post draw callbacks bracket the GL queries, so the
            numbers describe everything the pass submits. Draw-call counting is not available for
            a camera scope (there is no camera-level list of submitted draws); use a NODE scope
            for that.
          - NODE scope: the subgraph's root StateSet is moved into a dedicated render bin
            ("PerfBin"). That bin's drawImplementation() brackets the GL queries AND exposes the
            exact list of render leaves submitted this frame, which gives the draw-call,
            instance and per-pass numbers.

        Collected metrics (each one can be masked out with 'flags'):
          GPU time (ms)          GL_TIMESTAMP pairs (falls back to GL_TIME_ELAPSED)
          draw calls             number of glDraw* issued by the watched subgraph
          primitives             GL_PRIMITIVES_SUBMITTED_ARB
          VS / FS invocations    GL_VERTEX_SHADER_INVOCATIONS_ARB / GL_FRAGMENT_SHADER_INVOCATIONS_ARB
          samples passed         GL_SAMPLES_PASSED (occlusion query)
          geometry (MB)          all vertex attributes + indices reachable from the subgraph
          textures (MB)          all textures in the subgraph's effective StateSets
          exclusive textures     textures referenced ONLY by this scope (needs the 'root' argument)
          other buffers (MB)     BufferData of TextureBuffer / UniformBufferBinding in those StateSets

        Accuracy / cost notes:
          - Queries are asynchronous: values are read back 2 frames later and never block the
            draw thread. There is no glFinish() / glGetQueryObject() busy waiting anywhere.
          - GL forbids two active queries on the same target, so nested scopes are handled by
            "split and accumulate": the outer query is ended when an inner scope starts and a new
            one is begun when it ends; the outer result is the sum of its segments. Outer scopes
            therefore report the numbers EXCLUDING their nested scopes, which is what a node scope
            should report; nested results are added back for the outer scope's pass summary.
          - Resource numbers are CPU-side estimates taken from the scene graph, refreshed every
            'setResourceInterval()' frames (default 60) so that a normal frame is not affected.
          - Moving a subgraph into its own bin changes the draw order inside the pass: harmless
            for opaque geometry, visible for transparent geometry. For transparent subgraphs use
            a flags mask without the query bits, or accept the reorder.
          - GL2 / GLES / WebGL contexts without the required extensions simply report 0 for the
            unsupported metrics; nothing in the rendering path changes.

        Typical use:
        @code
        osgVerse::PerfProfiler* p = osgVerse::PerfProfiler::instance();
        p->watchCamera(stage->camera.get(), "GBuffer", osgVerse::PerfProfiler::COUNTERS);
        p->watchNode(sceneRoot.get(), "City", osgVerse::PerfProfiler::ALL, root.get());
        ... each frame / on demand ...
        p->print(std::cout);
        @endcode
    */
    class PerfProfiler : public osg::Referenced
    {
    public:
        enum Flag
        {
            GPU_TIME = 0x0001,            // GL timestamp queries around the scope
            PIPELINE_STATS = 0x0002,      // primitives / VS / FS invocation counters
            OCCLUSION = 0x0004,           // GL_SAMPLES_PASSED
            DRAW_CALLS = 0x0008,          // glDraw* count (node scopes only)
            GEOMETRY = 0x0010,            // vertex attributes + indices (MB)
            TEXTURES = 0x0020,            // textures (MB)
            TEXTURE_EXCLUSIVE = 0x0040,   // textures used by this scope only (needs root)
            BUFFERS = 0x0080,             // other BufferData (TBO / UBO ...) (MB)
            COUNTERS = GPU_TIME | PIPELINE_STATS | OCCLUSION | DRAW_CALLS,
            RESOURCES = GEOMETRY | TEXTURES | TEXTURE_EXCLUSIVE | BUFFERS,
            ALL = 0xffff
        };

        /** Statistics of one scope for one frame, optionally split per pass (camera name). */
        struct Sample
        {
            Sample();
            void reset();

            int frameNumber;                  // frame these numbers belong to (-1: none yet)
            double gpuMs, cpuMs;              // GPU / CPU (draw submission) time
            unsigned int drawCalls, drawables, instances;
            unsigned int pixels;              // viewport pixels, for overdraw / coverage
            unsigned long long primitives, vsInvocations, fsInvocations, samplesPassed;
            unsigned long long geometryBytes, textureBytes, texExclusiveBytes, bufferBytes;
            unsigned int geometries, textures, texturesExclusive;
            unsigned int texturesUnknown;     // textures without CPU-side image data (size unknown)
        };

        typedef std::map<std::string, Sample> PassSamples;
        struct Result
        {
            std::string name;
            int flags;
            bool cameraScope;
            bool exclusiveSupported;
            Sample total;                     // sum of all passes, inclusive of nested scopes
            PassSamples passes;               // per camera name
        };

        static PerfProfiler* instance();

        /** Watch a whole camera / pass. Returns false if the camera is invalid or the name is busy. */
        bool watchCamera(osg::Camera* camera, const std::string& name, int flags = ALL);

        /** Watch a node / subgraph.
            'root' is only needed for TEXTURE_EXCLUSIVE: it is the scene root used to find out
            whether a texture is also referenced outside of this scope (pass NULL to disable it).
            'binNumber' chooses the isolated render bin; <= 0 means "allocate one automatically". */
        bool watchNode(osg::Node* node, const std::string& name, int flags = ALL,
                       osg::Node* root = NULL, int binNumber = -1);

        /** Stop watching (restores the original cull callback / render bin settings). */
        bool unwatch(const std::string& name);
        void unwatchAll();

        /** Turn collection on/off without detaching; the scope keeps its slot and history. */
        bool setEnabled(const std::string& name, bool enabled);
        void setEnabled(bool enabled);

        /** How many frames between two resource recalculations (default 60, 0 = every frame). */
        void setResourceInterval(int frames);

        /** How many published frames are kept per scope for baselines / spike detection. */
        void setHistorySize(int frames);
        std::vector<Sample> getHistory(const std::string& name) const;

        /** Driver-reported GPU memory (currently only GL_NVX_gpu_memory_info). It is sampled a few
            times per second while scopes are collected; all values stay 0 when the driver does not
            expose it. */
        struct GpuMemoryInfo
        {
            bool supported;
            unsigned long long totalBytes, availableBytes;
            GpuMemoryInfo() : supported(false), totalBytes(0), availableBytes(0) {}
            unsigned long long getUsedBytes() const
            { return (totalBytes > availableBytes) ? totalBytes - availableBytes : 0; }
        };

        /** Names of all watched scopes, in attach order. */
        std::vector<std::string> getScopeNames() const;

        /** Latest GPU memory sample; 'supported' is false when the driver does not report it. */
        GpuMemoryInfo getGpuMemoryInfo() const;

        /** Latest result of a scope, or NULL if the name is unknown. */
        const Result* getResult(const std::string& name) const;

        /** One line per scope (name | pass | values). */
        void print(std::ostream& out) const;
        std::string toString() const;

    protected:
        friend struct PerfProfilerImpl;
        PerfProfiler();
        virtual ~PerfProfiler();

        PerfProfilerImpl* _impl;
    };
}

#endif
