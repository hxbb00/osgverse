#include "PerfProfiler.h"
#include <osg/GL>
#include <osg/Array>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/State>
#include <osg/StateSet>
#include <osg/Texture>
#include <osg/TextureBuffer>
#include <osg/Timer>
#include <osg/FrameStamp>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderLeaf>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>
#include <osgUtil/Statistics>
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

// GL query enums / entry points only exist on desktop GL builds. On GLES / WebGL the whole
// query path is compiled out and the corresponding metrics are simply reported as 0.
#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
#   define VERSE_PERF_QUERIES 1
#else
#   define VERSE_PERF_QUERIES 0
#endif

namespace osgVerse
{
    namespace
    {
        // GL allows only one active query per target, which is why nested scopes have to use
        // the split/accumulate logic in PerfProfilerImpl::begin() / end().
        enum TargetIndex { PRIM = 0, VS = 1, FS = 2, SAMPLES = 3, MAX_TARGETS = 4 };

        enum
        {
            F_GPU_TIME = PerfProfiler::GPU_TIME,
            F_PIPELINE_STATS = PerfProfiler::PIPELINE_STATS,
            F_OCCLUSION = PerfProfiler::OCCLUSION,
            F_DRAW_CALLS = PerfProfiler::DRAW_CALLS,
            F_GEOMETRY = PerfProfiler::GEOMETRY,
            F_TEXTURES = PerfProfiler::TEXTURES,
            F_TEX_EXCLUSIVE = PerfProfiler::TEXTURE_EXCLUSIVE,
            F_BUFFERS = PerfProfiler::BUFFERS
        };

#if VERSE_PERF_QUERIES
        const GLenum kTargetEnums[MAX_TARGETS] = {
            GL_PRIMITIVES_SUBMITTED_ARB, GL_VERTEX_SHADER_INVOCATIONS_ARB,
            GL_FRAGMENT_SHADER_INVOCATIONS_ARB, GL_SAMPLES_PASSED
        };
#endif

        std::string formatMegabytes(unsigned long long bytes)
        {
            char buf[64]; sprintf(buf, "%.2fMB", double(bytes) / (1024.0 * 1024.0));
            return std::string(buf);
        }
    }

    struct PerfProfilerImpl
    {
        typedef PerfProfiler::Sample Sample;
        typedef PerfProfiler::PassSamples PassSamples;
        typedef PerfProfiler::Result Result;

        struct Scope;
        struct Entry
        {
            Scope* scope; unsigned int contextID; std::string pass; int frame;
            osg::Timer_t cpuStart; double cpuMs;
            unsigned int drawCalls, drawables, instances;
            bool useTime, useTarget[MAX_TARGETS];
            unsigned int tsBegin, tsEnd;
            std::vector<unsigned int> segments[MAX_TARGETS];
            Entry* parent;

            Entry() : scope(NULL), contextID(0), frame(-1), cpuStart(0), cpuMs(0.0),
                      drawCalls(0), drawables(0), instances(0), useTime(false), tsBegin(0), tsEnd(0),
                      parent(NULL)
            {
                for (int i = 0; i < MAX_TARGETS; ++i) useTarget[i] = false;
            }
        };

        struct ResourceCache
        {
            unsigned long long geometryBytes, textureBytes, bufferBytes, exclusiveBytes;
            unsigned int geometries, textures, exclusiveTextures;
            int frame;

            ResourceCache() : geometryBytes(0), textureBytes(0), bufferBytes(0), exclusiveBytes(0),
                              geometries(0), textures(0), exclusiveTextures(0), frame(-100000) {}
        };

        struct Scope : public osg::Referenced
        {
            std::string name; int flags; bool cameraScope, enabled, exclusiveSupported;
            osg::observer_ptr<osg::Camera> camera;
            osg::observer_ptr<osg::Node> node, root;
            int binNumber;
            osg::observer_ptr<osg::StateSet> taggedSet;
            osg::StateSet::RenderBinMode oldMode; int oldNumber; std::string oldName;
            osg::ref_ptr<osg::Callback> oldCullCallback;   // node scope: previous cull callback
            osg::observer_ptr<osgUtil::RenderStage> renderStage;  // camera scope: cull result
            osg::ref_ptr<osg::Camera::DrawCallback> oldPreDraw, oldPostDraw;
            Sample last; PassSamples lastPasses;
            ResourceCache resources; Result cached;

            Scope() : flags(0), cameraScope(false), enabled(true), exclusiveSupported(false),
                      binNumber(-1), oldMode(osg::StateSet::INHERIT_RENDERBIN_DETAILS),
                      oldNumber(0) {}
        };

        struct FrameAccum
        {
            int frame; Sample total; PassSamples passes;
            FrameAccum() : frame(-1) {}
        };

        struct Context
        {
            std::vector<Entry*> stack, pending;
            unsigned int activeQuery[MAX_TARGETS];
            Entry* activeOwner[MAX_TARGETS];

            Context()
            {
                for (int i = 0; i < MAX_TARGETS; ++i)
                { activeQuery[i] = 0; activeOwner[i] = NULL; }
            }
        };

        std::map<unsigned int, Context> contexts;
        std::map<std::string, FrameAccum> accums;
        std::map<int, Scope*> bins;
        std::vector<osg::ref_ptr<Scope> > scopes;
        bool enabled;
        int nextBinNumber;
        int resourceInterval;

        PerfProfilerImpl() : enabled(true), nextBinNumber(1000), resourceInterval(60) {}

        Scope* findScope(const std::string& name)
        {
            for (size_t i = 0; i < scopes.size(); ++i)
                if (scopes[i]->name == name) return scopes[i].get();
            return NULL;
        }

        // ---------------------------------------------------------------- GPU queries
        static bool isQuerySupported(osg::GLExtensions* ext)
        {
#if VERSE_PERF_QUERIES
            return ext && ext->glGenQueries && ext->glBeginQuery && ext->glEndQuery &&
                   ext->glDeleteQueries && ext->glGetQueryObjectuiv;
#else
            (void)ext; return false;
#endif
        }

        static bool isTimeSupported(osg::GLExtensions* ext)
        {
#if VERSE_PERF_QUERIES
            return isQuerySupported(ext) && ext->isTimerQuerySupported && ext->glQueryCounter &&
                   ext->glGetQueryObjectui64v;
#else
            (void)ext; return false;
#endif
        }

        static bool isStatSupported(osg::GLExtensions* ext, unsigned int contextID)
        {
#if VERSE_PERF_QUERIES
            if (!isQuerySupported(ext)) return false;
            return ext->glVersion >= 4.4f ||
                   osg::isGLExtensionSupported(contextID, "GL_ARB_pipeline_statistics_query");
#else
            (void)ext; (void)contextID; return false;
#endif
        }

        static unsigned int createQuery(osg::GLExtensions* ext)
        {
#if VERSE_PERF_QUERIES
            GLuint q = 0; ext->glGenQueries(1, &q); return q;
#else
            (void)ext; return 0;
#endif
        }

        static void deleteQueries(osg::GLExtensions* ext, std::vector<unsigned int>& list)
        {
#if VERSE_PERF_QUERIES
            if (!list.empty() && ext) ext->glDeleteQueries((GLsizei)list.size(), &list[0]);
#else
            (void)ext; (void)list;
#endif
        }

        Entry* begin(osg::RenderInfo& ri, Scope* s, const std::string& passName)
        {
            if (!enabled || !s->enabled) return NULL;
            osg::State* state = ri.getState(); if (!state) return NULL;

            unsigned int contextID = ri.getContextID();
            osg::GLExtensions* ext = state->get<osg::GLExtensions>();
            int frame = state->getFrameStamp() ? (int)state->getFrameStamp()->getFrameNumber() : -1;
            poll(contextID, frame, ext);

            Entry* e = new Entry;
            e->scope = s; e->contextID = contextID; e->pass = passName;
            e->frame = frame; e->cpuStart = osg::Timer::instance()->tick();
            e->useTime = (s->flags & F_GPU_TIME) != 0 && isTimeSupported(ext);
            e->useTarget[PRIM] = e->useTarget[VS] = e->useTarget[FS] =
                (s->flags & F_PIPELINE_STATS) != 0 && isStatSupported(ext, contextID);
            e->useTarget[SAMPLES] = (s->flags & F_OCCLUSION) != 0 &&
                isQuerySupported(ext) && ext->isOcclusionQuerySupported;

            Context& ctx = contexts[contextID];
            e->parent = ctx.stack.empty() ? NULL : ctx.stack.back();
            ctx.stack.push_back(e);

#if VERSE_PERF_QUERIES
            if (e->useTime)
            {
                e->tsBegin = createQuery(ext); e->tsEnd = createQuery(ext);
                ext->glQueryCounter(e->tsBegin, GL_TIMESTAMP);
            }
            for (int t = 0; t < MAX_TARGETS; ++t)
            {
                if (!e->useTarget[t]) continue;
                if (ctx.activeQuery[t])   // an outer scope owns the running query: split it
                {
                    ext->glEndQuery(kTargetEnums[t]);
                    Entry* owner = ctx.activeOwner[t];
                    if (owner) owner->segments[t].push_back(ctx.activeQuery[t]);
                    ctx.activeQuery[t] = 0; ctx.activeOwner[t] = NULL;
                }
                unsigned int q = createQuery(ext);
                ext->glBeginQuery(kTargetEnums[t], q);
                ctx.activeQuery[t] = q; ctx.activeOwner[t] = e;
            }
#endif
            return e;
        }

        static void addDrawableCounts(const osgUtil::RenderLeaf* leaf, unsigned int& drawCalls,
                                      unsigned int& instances)
        {
            const osg::Drawable* drawable = leaf ? leaf->getDrawable() : NULL;
            if (!drawable) return;
            const osg::Geometry* geom = dynamic_cast<const osg::Geometry*>(drawable);
            if (!geom) { ++drawCalls; return; }  // ShapeDrawable / text / custom drawable

            for (unsigned int i = 0; i < geom->getNumPrimitiveSets(); ++i)
            {
                const osg::PrimitiveSet* pset = geom->getPrimitiveSet(i);
                if (!pset || pset->getNumIndices() == 0) continue;
                const osg::DrawArrayLengths* lengths =
                    dynamic_cast<const osg::DrawArrayLengths*>(pset);
                drawCalls += lengths ? std::max<unsigned int>(1, (unsigned int)lengths->size()) : 1;
                if (pset->getNumInstances() > 0) instances += (unsigned int)pset->getNumInstances();
            }
        }

        void end(osg::RenderInfo& ri, Scope* s)
        {
            if (!enabled || !s->enabled) return;
            osg::State* state = ri.getState(); if (!state) return;

            unsigned int contextID = ri.getContextID();
            osg::GLExtensions* ext = state->get<osg::GLExtensions>();
            Context& ctx = contexts[contextID];
            if (ctx.stack.empty()) return;

            Entry* e = ctx.stack.back();
            if (e->scope != s)   // unbalanced (scope removed mid-frame?): look for it
            {
                e = NULL;
                for (size_t i = ctx.stack.size(); i > 0; --i)
                    if (ctx.stack[i - 1]->scope == s) { e = ctx.stack[i - 1]; break; }
                if (!e) return;
                ctx.stack.erase(std::find(ctx.stack.begin(), ctx.stack.end(), e));
            }
            else
                ctx.stack.pop_back();

            e->cpuMs = osg::Timer::instance()->delta_m(e->cpuStart,
                                                       osg::Timer::instance()->tick());

#if VERSE_PERF_QUERIES
            if (e->useTime) ext->glQueryCounter(e->tsEnd, GL_TIMESTAMP);
            for (int t = 0; t < MAX_TARGETS; ++t)
            {
                if (!e->useTarget[t]) continue;
                if (ctx.activeQuery[t] && ctx.activeOwner[t] == e)
                {
                    ext->glEndQuery(kTargetEnums[t]);
                    e->segments[t].push_back(ctx.activeQuery[t]);
                    ctx.activeQuery[t] = 0; ctx.activeOwner[t] = NULL;

                    Entry* outer = e->parent;   // resume the outer scope with a new segment
                    if (outer && outer->useTarget[t])
                    {
                        unsigned int q = createQuery(ext);
                        ext->glBeginQuery(kTargetEnums[t], q);
                        ctx.activeQuery[t] = q; ctx.activeOwner[t] = outer;
                    }
                }
            }
#endif
            ctx.pending.push_back(e);
        }

        // Read back everything older than 2 frames; never blocks the draw thread.
        void poll(unsigned int contextID, int frame, osg::GLExtensions* ext)
        {
            Context& ctx = contexts[contextID];
            if (ctx.pending.empty()) return;

            size_t ready = 0;
            for (; ready < ctx.pending.size(); ++ready)
            {
                Entry* e = ctx.pending[ready];
                if (frame >= 0 && e->frame >= 0 && frame - e->frame < 2) break;
                finishEntry(e, ext);
                delete e;
            }
            if (ready > 0) ctx.pending.erase(ctx.pending.begin(), ctx.pending.begin() + ready);
        }

        void finishEntry(Entry* e, osg::GLExtensions* ext)
        {
            Sample sample; sample.frameNumber = e->frame;
            sample.cpuMs = e->cpuMs; sample.drawCalls = e->drawCalls;
            sample.drawables = e->drawables; sample.instances = e->instances;

#if VERSE_PERF_QUERIES
            if (ext)
            {
                if (e->useTime && e->tsBegin && e->tsEnd)
                {
                    std::vector<unsigned int> ids; ids.push_back(e->tsBegin); ids.push_back(e->tsEnd);
                    GLuint avail = 0;
                    ext->glGetQueryObjectuiv(e->tsEnd, GL_QUERY_RESULT_AVAILABLE, &avail);
                    if (avail)
                    {
                        GLuint64 begin = 0, end = 0;
                        ext->glGetQueryObjectui64v(e->tsBegin, GL_QUERY_RESULT, &begin);
                        ext->glGetQueryObjectui64v(e->tsEnd, GL_QUERY_RESULT, &end);
                        if (end >= begin) sample.gpuMs = double(end - begin) * 1e-6;
                    }
                    deleteQueries(ext, ids);
                }

                unsigned long long* values[MAX_TARGETS] = {
                    &sample.primitives, &sample.vsInvocations,
                    &sample.fsInvocations, &sample.samplesPassed };
                for (int t = 0; t < MAX_TARGETS; ++t)
                {
                    if (!e->useTarget[t]) continue;
                    for (size_t i = 0; i < e->segments[t].size(); ++i)
                    {
                        GLuint value = 0;
                        ext->glGetQueryObjectuiv(e->segments[t][i], GL_QUERY_RESULT, &value);
                        (*values[t]) += value;
                    }
                    deleteQueries(ext, e->segments[t]);
                }
            }
#endif
            publish(e, sample);
        }

        void publishInto(Scope* s, int frame, const std::string& pass, const Sample& sample)
        {
            FrameAccum& acc = accums[s->name];
            if (frame > acc.frame)
            {
                if (acc.frame >= 0)   // the previous frame has been fully accumulated
                { s->last = acc.total; s->lastPasses = acc.passes; }
                acc.frame = frame; acc.total.reset(); acc.total.frameNumber = frame;
                acc.passes.clear();
            }
            else if (frame < acc.frame) return;   // too late, dropped
            merge(acc.total, sample);
            merge(acc.passes[pass], sample);
        }

        void publish(Entry* e, const Sample& sample)
        {
            publishInto(e->scope, e->frame, e->pass, sample);

            // Outer scopes were split while this entry was active, so their own GL counters
            // exclude it: add those back to make camera/pass totals inclusive. CPU time and draw
            // calls are NOT inherited, an outer scope measures them on its own.
            Sample inherited;
            inherited.frameNumber = sample.frameNumber; inherited.gpuMs = sample.gpuMs;
            inherited.primitives = sample.primitives; inherited.vsInvocations = sample.vsInvocations;
            inherited.fsInvocations = sample.fsInvocations; inherited.samplesPassed = sample.samplesPassed;
            for (Entry* p = e->parent; p; p = p->parent)
                publishInto(p->scope, e->frame, p->pass, inherited);
        }

        static void merge(Sample& dst, const Sample& src)
        {
            dst.frameNumber = std::max(dst.frameNumber, src.frameNumber);
            dst.gpuMs += src.gpuMs; dst.cpuMs += src.cpuMs;
            dst.drawCalls += src.drawCalls; dst.drawables += src.drawables;
            dst.instances += src.instances;
            dst.primitives += src.primitives; dst.vsInvocations += src.vsInvocations;
            dst.fsInvocations += src.fsInvocations; dst.samplesPassed += src.samplesPassed;
        }

        // ------------------------------------------------------------- resources
        struct ResourceGather
        {
            std::set<const osg::BufferData*> dataSeen;
            std::set<const osg::Texture*> textureSeen;
            std::vector<const osg::StateSet*> stack;
            unsigned long long geometryBytes, textureBytes, bufferBytes;
            unsigned int geometries;

            ResourceGather() : geometryBytes(0), textureBytes(0), bufferBytes(0), geometries(0) {}

            static unsigned long long textureSize(const osg::Texture* tex)
            {
                unsigned long long bytes = 0;
                for (unsigned int i = 0; i < tex->getNumImages(); ++i)
                {
                    const osg::Image* img = tex->getImage(i);
                    if (img) bytes += img->getTotalDataSize();
                }
                return bytes;
            }

            void addArray(const osg::Array* array)
            {
                if (!array || !dataSeen.insert(array).second) return;
                geometryBytes += array->getTotalDataSize();
            }

            void addTexturesAndBuffers()
            {
                // Innermost StateSet wins, exactly like the GL state stack does.
                std::set<unsigned int> usedUnits;
                for (size_t i = stack.size(); i > 0; --i)
                {
                    const osg::StateSet* ss = stack[i - 1];
                    for (unsigned int u = 0; u < ss->getNumTextureAttributeLists(); ++u)
                    {
                        if (!usedUnits.insert(u).second) continue;
                        const osg::StateAttribute* sa =
                            ss->getTextureAttribute(u, osg::StateAttribute::TEXTURE);
                        const osg::Texture* tex = dynamic_cast<const osg::Texture*>(sa);
                        if (tex && textureSeen.insert(tex).second) textureBytes += textureSize(tex);
                    }
                    const osg::StateSet::AttributeList& attrs = ss->getAttributeList();
                    for (osg::StateSet::AttributeList::const_iterator it = attrs.begin();
                         it != attrs.end(); ++it)
                    {
                        const osg::TextureBuffer* tbo =
                            dynamic_cast<const osg::TextureBuffer*>(it->second.first.get());
                        if (!tbo) continue;
                        const osg::BufferData* data = tbo->getBufferData();
                        if (data && dataSeen.insert(data).second)
                            bufferBytes += data->getTotalDataSize();
                    }
                }
            }

            void handleDrawable(const osg::Drawable* drawable)
            {
                // Materials (and therefore textures) usually live on the drawable's own StateSet,
                // so it has to become the innermost entry of the state stack.
                size_t mark = stack.size();
                if (drawable->getStateSet()) stack.push_back(drawable->getStateSet());

                const osg::Geometry* geom = dynamic_cast<const osg::Geometry*>(drawable);
                if (geom)
                {
                    ++geometries;
                    addArray(geom->getVertexArray());
                    for (unsigned int i = 0; i < geom->getNumVertexAttribArrays(); ++i)
                        addArray(geom->getVertexAttribArray(i));
                    for (unsigned int i = 0; i < geom->getNumPrimitiveSets(); ++i)
                    {
                        const osg::PrimitiveSet* pset = geom->getPrimitiveSet(i);
                        if (pset && dataSeen.insert(pset).second)
                            geometryBytes += pset->getTotalDataSize();
                    }
                }
                addTexturesAndBuffers();
                stack.resize(mark);
            }

            void traverse(osg::Node* node)
            {
                if (!node) return;
                size_t mark = stack.size();
                if (node->getStateSet()) stack.push_back(node->getStateSet());

                osg::Geode* geode = node->asGeode();
                if (geode)
                {
                    for (unsigned int i = 0; i < geode->getNumDrawables(); ++i)
                        handleDrawable(geode->getDrawable(i));
                }
                else
                {
                    osg::Group* group = node->asGroup();
                    if (group)
                        for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                            traverse(group->getChild(i));
                }
                stack.resize(mark);
            }
        };

        // For every texture under the root: how many drawables reference it, and how many of
        // those are inside the scope. Equal numbers means the texture is exclusive to the scope.
        struct TextureUsageGather
        {
            struct Usage { int total; std::map<const Scope*, int> perScope; Usage() : total(0) {} };
            std::map<const osg::Texture*, Usage> usage;
            std::vector<const osg::StateSet*> stack;
            const Scope* current;
            std::map<const osg::Node*, Scope*> watched;

            TextureUsageGather() : current(NULL) {}

            void gatherTextures(const osg::Drawable* drawable)
            {
                size_t mark = stack.size();
                if (drawable && drawable->getStateSet()) stack.push_back(drawable->getStateSet());

                std::set<unsigned int> usedUnits;
                for (size_t i = stack.size(); i > 0; --i)
                {
                    const osg::StateSet* ss = stack[i - 1];
                    for (unsigned int u = 0; u < ss->getNumTextureAttributeLists(); ++u)
                    {
                        if (!usedUnits.insert(u).second) continue;
                        const osg::StateAttribute* sa =
                            ss->getTextureAttribute(u, osg::StateAttribute::TEXTURE);
                        const osg::Texture* tex = dynamic_cast<const osg::Texture*>(sa);
                        if (!tex) continue;
                        Usage& info = usage[tex]; ++info.total;
                        if (current) ++info.perScope[current];
                    }
                }
                stack.resize(mark);
            }

            void traverse(osg::Node* node)
            {
                if (!node) return;
                const Scope* previous = current;
                std::map<const osg::Node*, Scope*>::iterator it = watched.find(node);
                if (it != watched.end()) current = it->second;

                size_t mark = stack.size();
                if (node->getStateSet()) stack.push_back(node->getStateSet());

                osg::Geode* geode = node->asGeode();
                if (geode)
                {
                    for (unsigned int i = 0; i < geode->getNumDrawables(); ++i)
                        if (geode->getDrawable(i)) gatherTextures(geode->getDrawable(i));
                }
                else
                {
                    osg::Group* group = node->asGroup();
                    if (group)
                        for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                            traverse(group->getChild(i));
                }
                stack.resize(mark); current = previous;
            }
        };

        void refreshResources(Scope* s, int frame)
        {
            if (!(s->flags & (F_GEOMETRY | F_TEXTURES | F_BUFFERS | F_TEX_EXCLUSIVE))) return;
            if (frame >= 0 && s->resources.frame >= 0 &&
                frame - s->resources.frame < resourceInterval) return;

            s->resources.frame = frame;
            s->resources.geometryBytes = s->resources.textureBytes = 0;
            s->resources.bufferBytes = s->resources.exclusiveBytes = 0;
            s->resources.geometries = s->resources.textures = s->resources.exclusiveTextures = 0;
            if (!s->node.valid()) return;

            ResourceGather gather; gather.traverse(s->node.get());
            s->resources.geometryBytes = gather.geometryBytes;
            s->resources.textureBytes = gather.textureBytes;
            s->resources.bufferBytes = gather.bufferBytes;
            s->resources.geometries = gather.geometries;
            s->resources.textures = (unsigned int)gather.textureSeen.size();

            if (!s->exclusiveSupported || !s->root.valid()) return;
            TextureUsageGather usage;
            for (size_t i = 0; i < scopes.size(); ++i)
            {
                Scope* other = scopes[i].get();
                if (other->cameraScope || !other->node.valid() || other->root != s->root) continue;
                usage.watched[other->node.get()] = other;
            }
            usage.traverse(s->root.get());
            for (std::map<const osg::Texture*, TextureUsageGather::Usage>::iterator
                 it = usage.usage.begin(); it != usage.usage.end(); ++it)
            {
                std::map<const Scope*, int>::iterator mine = it->second.perScope.find(s);
                if (mine == it->second.perScope.end()) continue;
                if (mine->second != it->second.total) continue;   // used outside this scope too
                s->resources.exclusiveBytes += ResourceGather::textureSize(it->first);
                ++s->resources.exclusiveTextures;
            }
        }

        static void applyResources(const Scope* s, Sample& sample)
        {
            sample.geometryBytes = s->resources.geometryBytes;
            sample.textureBytes = s->resources.textureBytes;
            sample.bufferBytes = s->resources.bufferBytes;
            sample.texExclusiveBytes = s->resources.exclusiveBytes;
            sample.geometries = s->resources.geometries;
            sample.textures = s->resources.textures;
            sample.texturesExclusive = s->resources.exclusiveTextures;
        }

        // ------------------------------------------------------------- OSG hooks
        struct PerfBin : public osgUtil::RenderBin
        {
            PerfBin() {}
            PerfBin(const PerfBin& rhs, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
                : osgUtil::RenderBin(rhs, copyop) {}

            osg::Object* cloneType() const { return new PerfBin; }
            osg::Object* clone(const osg::CopyOp& copyop) const { return new PerfBin(*this, copyop); }

            virtual void drawImplementation(osg::RenderInfo& ri, osgUtil::RenderLeaf*& previous)
            {
                PerfProfilerImpl* impl = PerfProfiler::instance()->_impl;
                Scope* scope = NULL;
                std::map<int, Scope*>::iterator it = impl->bins.find(getBinNum());
                if (it != impl->bins.end()) scope = it->second;

                unsigned int drawCalls = 0, instances = 0;
                Entry* entry = NULL;
                if (scope)
                {
                    // Flattening keeps the exact coarse-grained order of the state graph, but
                    // leaves us with the list of leaves submitted this frame.
                    if (_renderLeafList.empty()) copyLeavesFromStateGraphListToRenderLeafList();
                    for (size_t i = 0; i < _renderLeafList.size(); ++i)
                        impl->addDrawableCounts(_renderLeafList[i], drawCalls, instances);

                    osg::State* state = ri.getState();
                    int frame = (state && state->getFrameStamp())
                              ? (int)state->getFrameStamp()->getFrameNumber() : -1;
                    impl->refreshResources(scope, frame);
                    osg::Camera* cam = ri.getCurrentCamera();
                    entry = impl->begin(ri, scope, cam ? cam->getName() : "pass");
                    if (entry)
                    {
                        entry->drawCalls = drawCalls; entry->instances = instances;
                        entry->drawables = (unsigned int)_renderLeafList.size();
                    }
                }
                osgUtil::RenderBin::drawImplementation(ri, previous);
                if (entry) impl->end(ri, scope);
            }
        };

        struct PassCallback : public osg::Camera::DrawCallback
        {
            PerfProfilerImpl* impl; Scope* scope; bool isBegin;
            PassCallback(PerfProfilerImpl* i, Scope* s, bool b) : impl(i), scope(s), isBegin(b) {}

            virtual void operator()(osg::RenderInfo& ri) const
            {
                osg::State* state = ri.getState();
                int frame = (state && state->getFrameStamp())
                          ? (int)state->getFrameStamp()->getFrameNumber() : -1;
                if (isBegin)
                {
                    impl->refreshResources(scope, frame);
                    osg::Camera* cam = ri.getCurrentCamera();
                    Entry* e = impl->begin(ri, scope, cam ? cam->getName() : "pass");
                    if (e && scope->renderStage.valid())
                    {   // the cull result is complete here: count what this pass will submit
                        osgUtil::Statistics stats;
                        if (scope->renderStage->getStats(stats))
                            e->drawables = (unsigned int)stats.numDrawables;
                    }
                }
                else
                    impl->end(ri, scope);
            }
        };

        struct NodeCullCallback : public osg::NodeCallback
        {
            PerfProfilerImpl* impl; Scope* scope;
            NodeCullCallback(PerfProfilerImpl* i, Scope* s) : impl(i), scope(s) {}

            virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
            {
                if (scope->oldCullCallback.valid())
                    scope->oldCullCallback->run(node, nv);
                else
                    osg::NodeCallback::traverse(node, nv);
            }
        };

        struct CameraCullCallback : public osg::NodeCallback
        {
            PerfProfilerImpl* impl; Scope* scope;
            CameraCullCallback(PerfProfilerImpl* i, Scope* s) : impl(i), scope(s) {}

            virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
            {
                osgUtil::CullVisitor* cv = dynamic_cast<osgUtil::CullVisitor*>(nv);
                if (cv) scope->renderStage = cv->getRenderStage();
                if (scope->oldCullCallback.valid()) scope->oldCullCallback->run(node, nv);
                else osg::NodeCallback::traverse(node, nv);
            }
        };

        // ------------------------------------------------------------- setup
        bool watchCamera(osg::Camera* camera, const std::string& name, int flags)
        {
            if (!camera || findScope(name)) return false;

            osg::ref_ptr<Scope> scope = new Scope;
            scope->name = name; scope->flags = flags; scope->cameraScope = true;
            scope->camera = camera;
            scope->oldCullCallback = camera->getCullCallback();
            camera->setCullCallback(new CameraCullCallback(this, scope.get()));
            scope->oldPreDraw = camera->getPreDrawCallback();
            scope->oldPostDraw = camera->getPostDrawCallback();
            camera->setPreDrawCallback(new PassCallback(this, scope.get(), true));
            camera->setPostDrawCallback(new PassCallback(this, scope.get(), false));
            scopes.push_back(scope);
            return true;
        }

        bool watchNode(osg::Node* node, const std::string& name, int flags,
                       osg::Node* root, int binNumber)
        {
            if (!node || findScope(name)) return false;

            osg::ref_ptr<Scope> scope = new Scope;
            scope->name = name; scope->flags = flags; scope->cameraScope = false;
            scope->node = node; scope->root = root;
            scope->exclusiveSupported = (flags & F_TEX_EXCLUSIVE) != 0 && root != NULL;
            if (binNumber <= 0) binNumber = nextBinNumber++;

            osg::StateSet* ss = node->getOrCreateStateSet();
            scope->taggedSet = ss;
            scope->oldMode = ss->getRenderBinMode();
            scope->oldNumber = ss->getBinNumber();
            scope->oldName = ss->getBinName();
            ss->setRenderBinDetails(binNumber, "PerfBin");
            scope->binNumber = binNumber;
            bins[binNumber] = scope.get();

            scope->oldCullCallback = node->getCullCallback();
            node->setCullCallback(new NodeCullCallback(this, scope.get()));
            scopes.push_back(scope);
            return true;
        }

        void dropEntries(Scope* scope)
        {
            // Pending entries reference the scope, so they have to go before it is released.
            for (std::map<unsigned int, Context>::iterator it = contexts.begin();
                 it != contexts.end(); ++it)
            {
                Context& ctx = it->second;
                for (int t = 0; t < MAX_TARGETS; ++t)
                {
                    if (ctx.activeOwner[t] && ctx.activeOwner[t]->scope == scope)
                        ctx.activeOwner[t] = NULL, ctx.activeQuery[t] = 0;
                }
                for (size_t i = 0; i < ctx.stack.size(); )
                {
                    if (ctx.stack[i]->scope == scope) { delete ctx.stack[i]; ctx.stack.erase(ctx.stack.begin() + i); }
                    else ++i;
                }
                for (size_t i = 0; i < ctx.pending.size(); )
                {
                    if (ctx.pending[i]->scope == scope) { delete ctx.pending[i]; ctx.pending.erase(ctx.pending.begin() + i); }
                    else ++i;
                }
            }
            accums.erase(scope->name);
        }

        bool unwatch(const std::string& name)
        {
            for (size_t i = 0; i < scopes.size(); ++i)
            {
                Scope* s = scopes[i].get();
                if (s->name != name) continue;

                dropEntries(s);
                if (s->cameraScope)
                {
                    if (s->camera.valid())
                    {
                        s->camera->setCullCallback(s->oldCullCallback.get());
                        s->camera->setPreDrawCallback(s->oldPreDraw.get());
                        s->camera->setPostDrawCallback(s->oldPostDraw.get());
                    }
                }
                else
                {
                    if (s->node.valid()) s->node->setCullCallback(s->oldCullCallback.get());
                    if (s->taggedSet.valid())
                    {
                        if (s->oldMode == osg::StateSet::USE_RENDERBIN_DETAILS)
                            s->taggedSet->setRenderBinDetails(s->oldNumber, s->oldName);
                        else
                            s->taggedSet->setRenderBinToInherit();
                    }
                    bins.erase(s->binNumber);
                }
                scopes.erase(scopes.begin() + i);
                return true;
            }
            return false;
        }

        const Result* buildResult(Scope* s)
        {
            s->cached.name = s->name; s->cached.flags = s->flags;
            s->cached.cameraScope = s->cameraScope;
            s->cached.exclusiveSupported = s->exclusiveSupported;
            s->cached.total = s->last; s->cached.passes = s->lastPasses;
            applyResources(s, s->cached.total);
            return &s->cached;
        }

        std::string describe(const Scope* s)
        {
            std::ostringstream out;
            Sample v = s->last; applyResources(s, v);
            out << "[Perf] " << s->name << (s->cameraScope ? " (camera)" : " (node)");
            if (!s->enabled) out << " [disabled]";
            out << " frame " << v.frameNumber << ": ";
            if (s->flags & F_GPU_TIME) out << "gpu " << v.gpuMs << "ms, ";
            out << "cpu " << v.cpuMs << "ms, ";
            if (s->flags & F_DRAW_CALLS)
            {
                if (s->cameraScope) out << "draw n/a, " << v.drawables << " drawables, ";
                else out << "draw " << v.drawCalls << " calls / " << v.drawables << " drawables, ";
                if (v.instances > 0) out << v.instances << " inst, ";
            }
            if (s->flags & F_PIPELINE_STATS)
                out << "prim " << v.primitives << " / vs " << v.vsInvocations
                    << " / fs " << v.fsInvocations << ", ";
            if (s->flags & F_OCCLUSION) out << "samples " << v.samplesPassed << ", ";
            if (s->flags & F_GEOMETRY)
                out << "geometry " << formatMegabytes(v.geometryBytes) << " (" << v.geometries << "), ";
            if (s->flags & F_TEXTURES)
                out << "textures " << formatMegabytes(v.textureBytes) << " (" << v.textures << "), ";
            if (s->flags & F_TEX_EXCLUSIVE)
            {
                if (s->exclusiveSupported)
                    out << "exclusive " << formatMegabytes(v.texExclusiveBytes) << " ("
                        << v.texturesExclusive << "), ";
                else
                    out << "exclusive n/a (no root), ";
            }
            if (s->flags & F_BUFFERS) out << "buffers " << formatMegabytes(v.bufferBytes);
            return out.str();
        }
    };

    // ----------------------------------------------------------------- PerfProfiler
    PerfProfiler::Sample::Sample() { reset(); }

    void PerfProfiler::Sample::reset()
    {
        frameNumber = -1; gpuMs = cpuMs = 0.0; drawCalls = drawables = instances = 0;
        primitives = vsInvocations = fsInvocations = samplesPassed = 0;
        geometryBytes = textureBytes = texExclusiveBytes = bufferBytes = 0;
        geometries = textures = texturesExclusive = 0;
    }

    PerfProfiler* PerfProfiler::instance()
    {
        static osg::ref_ptr<PerfProfiler> s_instance = new PerfProfiler;
        return s_instance.get();
    }

    PerfProfiler::PerfProfiler()
    {
        _impl = new PerfProfilerImpl;
        static bool s_registered = false;
        if (!s_registered)
        {
            osgUtil::RenderBin::addRenderBinPrototype("PerfBin", new PerfProfilerImpl::PerfBin);
            s_registered = true;
        }
    }

    PerfProfiler::~PerfProfiler()
    {
        unwatchAll();
        delete _impl;
    }

    bool PerfProfiler::watchCamera(osg::Camera* camera, const std::string& name, int flags)
    { return _impl->watchCamera(camera, name, flags); }

    bool PerfProfiler::watchNode(osg::Node* node, const std::string& name, int flags,
                                 osg::Node* root, int binNumber)
    { return _impl->watchNode(node, name, flags, root, binNumber); }

    bool PerfProfiler::unwatch(const std::string& name) { return _impl->unwatch(name); }

    void PerfProfiler::unwatchAll()
    {
        while (!_impl->scopes.empty())
            _impl->unwatch(_impl->scopes.back()->name);
    }

    bool PerfProfiler::setEnabled(const std::string& name, bool enabled)
    {
        PerfProfilerImpl::Scope* s = _impl->findScope(name);
        if (!s) return false;
        s->enabled = enabled; return true;
    }

    void PerfProfiler::setEnabled(bool enabled) { _impl->enabled = enabled; }

    void PerfProfiler::setResourceInterval(int frames) { _impl->resourceInterval = frames; }

    const PerfProfiler::Result* PerfProfiler::getResult(const std::string& name) const
    {
        PerfProfilerImpl::Scope* s = _impl->findScope(name);
        return s ? _impl->buildResult(s) : NULL;
    }

    void PerfProfiler::print(std::ostream& out) const
    {
        for (size_t i = 0; i < _impl->scopes.size(); ++i)
            out << _impl->describe(_impl->scopes[i].get()) << std::endl;
    }

    std::string PerfProfiler::toString() const
    {
        std::ostringstream out; print(out); return out.str();
    }
}
