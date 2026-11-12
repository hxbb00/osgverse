#include "PerfAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace osgVerse
{
    namespace
    {
        double median(std::vector<double> values)
        {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            size_t n = values.size();
            return (n % 2) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * 0.5;
        }

        std::string formatCount(double value)
        {
            char buf[64];
            if (value >= 1e9) sprintf(buf, "%.2fG", value / 1e9);
            else if (value >= 1e6) sprintf(buf, "%.2fM", value / 1e6);
            else if (value >= 1e3) sprintf(buf, "%.2fk", value / 1e3);
            else sprintf(buf, "%.0f", value);
            return std::string(buf);
        }

        std::string formatNumber(const char* fmt, double a)
        {
            char buf[96]; sprintf(buf, fmt, a); return std::string(buf);
        }

        // 0 at the threshold, 1 at 'full' (default 2x the threshold), clamped
        double ramp(double value, double threshold, double full = 0.0)
        {
            if (threshold <= 0.0) return 0.0;
            if (full <= 0.0) full = threshold * 2.0;
            if (value <= threshold) return 0.0;
            double t = (value - threshold) / std::max(full - threshold, 1e-9);
            return std::min(1.0, std::max(0.0, t));
        }
    }

    PerfAnalyzer::Thresholds::Thresholds()
        : minGpuMs(0.05), minPrimitives(2e4), minFragments(2e5), overdrawHigh(2.0),
          overdrawLow(1.3), wastedHigh(1.5), vertexRatioHigh(2.9), nsPerPrimitiveHeavy(5.0),
          nsPerFragmentHeavy(20.0), manyDrawCalls(500.0), tinyPrimitivesPerDraw(64.0),
          spikeRatio(1.5), peerRatio(2.0), resourceShare(0.5), bigTextureMB(512.0),
          bigGeometryMB(256.0), fixedCostMs(0.3), hugeNsPerDraw(20000.0)
    {}

    PerfAnalyzer::Metrics::Metrics()
        : gpuMs(0), share(0), historyMs(0), primitives(0), vertices(0), fragments(0),
          samples(0), draws(0), drawables(0), instances(0), overdraw(0), wasted(0), coverage(0),
          vsPerPrimitive(0),
          fragPerVertex(0), nsPerPrimitive(0), nsPerFragment(0), nsPerSample(0), nsPerDraw(0),
          primitivesPerDraw(0), geometryMB(0), textureMB(0), exclusiveMB(0), bufferMB(0), pixels(0),
          queryValid(false), hasSamples(false)
    {}

    PerfAnalyzer::GlobalReport::GlobalReport()
        : frameMs(-1.0), totalGpuMs(0.0), geometryBytes(0), textureBytes(0) {}

    PerfAnalyzer::GlobalReport PerfAnalyzer::analyzeGlobal(
        const PerfProfiler& profiler, double frameMs, const Thresholds& th)
    {
        GlobalReport report;
        report.frameMs = frameMs;

        // 1) Sum up the watched scopes. Node scopes are used for the sums: a camera scope covers
        //    the same draws again, and counting both would report more GPU time than the frame
        std::vector<std::string> names = profiler.getScopeNames();
        std::vector<double> peerTimes, cameraTimes;
        std::string slowName; double slowMs = 0.0, nodeGpuMs = 0.0;
        for (size_t i = 0; i < names.size(); ++i)
        {
            const PerfProfiler::Result* r = profiler.getResult(names[i]);
            if (!r) continue;
            const PerfProfiler::Sample& s = r->total;
            if (r->cameraScope) { cameraTimes.push_back(s.gpuMs); continue; }

            nodeGpuMs += s.gpuMs;
            report.geometryBytes += s.geometryBytes;
            report.textureBytes += s.textureBytes;
            peerTimes.push_back(s.gpuMs);
            if (s.gpuMs > slowMs) { slowMs = s.gpuMs; slowName = r->name; }
        }
        if (!peerTimes.empty())
            report.totalGpuMs = nodeGpuMs;
        else
            for (size_t i = 0; i < cameraTimes.size(); ++i) report.totalGpuMs += cameraTimes[i];
        report.gpuMemory = profiler.getGpuMemoryInfo();

        // 2) GPU memory pressure: no per-scope counter can show it, only the driver knows
        if (report.gpuMemory.supported && report.gpuMemory.totalBytes > 0)
        {
            const double totalMB = double(report.gpuMemory.totalBytes) / (1024.0 * 1024.0);
            const double availMB = double(report.gpuMemory.availableBytes) / (1024.0 * 1024.0);
            const double usedRatio = double(report.gpuMemory.getUsedBytes()) /
                                     double(report.gpuMemory.totalBytes);
            if (usedRatio > 0.9 || availMB < 256.0)
            {
                Finding f;
                f.code = "VRAM_PRESSURE";
                f.title = "GPU memory nearly exhausted: resources are paged in and out";
                f.detail = "VRAM " + formatNumber("%.0f", usedRatio * 100.0) + "% used, " +
                           formatNumber("%.0f", availMB) + " MB free of " +
                           formatNumber("%.0f", totalMB) + " MB; watched resources (shared ones "
                           "counted twice) geometry " +
                           formatNumber("%.0f", double(report.geometryBytes) / (1024.0 * 1024.0)) +
                           " MB, textures " +
                           formatNumber("%.0f", double(report.textureBytes) / (1024.0 * 1024.0)) +
                           " MB";
                f.score = std::min(1.0, 0.6 + ramp(usedRatio, 0.9, 1.0) * 0.4);
                report.findings.push_back(f);
            }
        }

        // 3) Frame time vs the sum of the profiled GPU times: the missing part is paging, uploads,
        //    shader compilation or driver synchronization, none of which is a render pass. A scope
        //    sum of 0 counts too: under GPU back-pressure the timer queries may not resolve at all
        if (frameMs > 33.0)
        {
            const double accounted = (report.totalGpuMs > 0.0) ? report.totalGpuMs / frameMs : 0.0;
            if (accounted < 0.5)
            {
                Finding f;
                f.code = "UNACCOUNTED_TIME";
                f.title = "most of the frame time is outside the profiled render passes";
                f.detail = "frame " + formatNumber("%.1f", frameMs) + " ms while the watched scopes "
                           "sum to " + formatNumber("%.2f", report.totalGpuMs) + " ms (" +
                           formatNumber("%.0f", accounted * 100.0) + "%); the rest is resource "
                           "paging/uploads, shader compilation, driver synchronization or an "
                           "unprofiled pass";
                if (report.totalGpuMs <= 0.0)
                    f.detail += "; the GPU timer queries did not resolve either, which is another "
                                "symptom of GPU back-pressure";
                f.score = std::min(1.0, 0.5 + (0.5 - accounted));
                report.findings.push_back(f);
            }
        }

        // 4) A single scope dominating the cost while its peers are cheap is the paging signature
        const double outlierRatio = std::max(4.0, th.peerRatio * 4.0);
        if (slowMs > 1.0 && peerTimes.size() > 2)
        {
            const double med = median(peerTimes);
            if (med > 0.0 && slowMs / med > outlierRatio)
            {
                Finding f;
                f.code = "GPU_OUTLIER";
                f.title = "one scope is much slower than the other watched scopes";
                f.detail = slowName + " needs " + formatNumber("%.2f", slowMs) +
                           " ms vs peer median " + formatNumber("%.3f", med) + " ms (" +
                           formatNumber("%.0f", slowMs / med) + "x); together with a full GPU "
                           "memory this means the resources of this scope are re-uploaded";
                f.score = std::min(1.0, 0.4 + ramp(slowMs / med, outlierRatio, outlierRatio * 4.0) * 0.5);
                report.findings.push_back(f);
            }
        }

        std::sort(report.findings.begin(), report.findings.end(),
                  [](const Finding& a, const Finding& b) { return a.score > b.score; });
        report.verdict = report.findings.empty() ? "balanced (frame time matches the profiled work)"
                                                 : report.findings[0].title;
        return report;
    }

    PerfAnalyzer::Metrics PerfAnalyzer::computeMetrics(
        const PerfProfiler::Result& result, const std::vector<PerfProfiler::Sample>& history,
        double totalGpuMs)
    {
        const PerfProfiler::Sample& s = result.total;
        Metrics m;
        m.gpuMs = s.gpuMs;
        m.share = (totalGpuMs > 0.0) ? s.gpuMs / totalGpuMs : 0.0;
        m.primitives = double(s.primitives); m.vertices = double(s.vsInvocations);
        m.fragments = double(s.fsInvocations); m.samples = double(s.samplesPassed);
        m.draws = double(s.drawCalls); m.drawables = double(s.drawables);
        m.instances = double(s.instances);

        // Any of the counters being non-zero means the queries are available on this context.
        m.queryValid = (s.primitives || s.vsInvocations || s.fsInvocations || s.samplesPassed) != 0;
        m.hasSamples = s.samplesPassed > 0;

        m.wasted = (s.samplesPassed > 0) ? double(s.fsInvocations) / double(s.samplesPassed) : 0.0;
        if (s.pixels > 0)
        {
            m.overdraw = double(s.fsInvocations) / double(s.pixels);
            m.coverage = double(s.samplesPassed) / double(s.pixels);
        }
        m.vsPerPrimitive = (s.primitives > 0) ? double(s.vsInvocations) / double(s.primitives) : 0.0;
        m.fragPerVertex = (s.vsInvocations > 0) ? double(s.fsInvocations) / double(s.vsInvocations) : 0.0;
        m.primitivesPerDraw = (s.drawCalls > 0) ? double(s.primitives) / double(s.drawCalls) : 0.0;

        const double ns = s.gpuMs * 1e6;
        if (s.primitives > 0) m.nsPerPrimitive = ns / double(s.primitives);
        if (s.fsInvocations > 0) m.nsPerFragment = ns / double(s.fsInvocations);
        if (s.samplesPassed > 0) m.nsPerSample = ns / double(s.samplesPassed);
        if (s.drawCalls > 0) m.nsPerDraw = ns / double(s.drawCalls);

        m.pixels = double(s.pixels);
        m.geometryMB = double(s.geometryBytes) / (1024.0 * 1024.0);
        m.textureMB = double(s.textureBytes) / (1024.0 * 1024.0);
        m.exclusiveMB = double(s.texExclusiveBytes) / (1024.0 * 1024.0);
        m.bufferMB = double(s.bufferBytes) / (1024.0 * 1024.0);

        std::vector<double> times;
        for (size_t i = 0; i < history.size(); ++i)
            if (history[i].gpuMs > 0.0) times.push_back(history[i].gpuMs);
        m.historyMs = median(times);
        return m;
    }

    std::vector<PerfAnalyzer::Report> PerfAnalyzer::analyze(
        const PerfProfiler& profiler, const Thresholds& th)
    {
        // 1) Collect the scopes and their derived metrics
        std::vector<std::string> names;
        std::vector<const PerfProfiler::Result*> results;
        std::vector<std::vector<PerfProfiler::Sample> > histories;
        std::vector<Report> reports;
        double totalGpuMs = 0.0;

        names = profiler.getScopeNames();

        for (size_t i = 0; i < names.size(); ++i)
        {
            const PerfProfiler::Result* r = profiler.getResult(names[i]);
            if (!r) continue;
            results.push_back(r);
            histories.push_back(profiler.getHistory(names[i]));
            totalGpuMs += r->total.gpuMs;
        }
        for (size_t i = 0; i < results.size(); ++i)
        {
            Report rep;
            rep.name = results[i]->name; rep.cameraScope = results[i]->cameraScope;
            rep.metrics = computeMetrics(*results[i], histories[i], totalGpuMs);
            reports.push_back(rep);
        }

        // 2) Peer medians (only scopes with valid counters and real work)
        std::vector<double> peerNsPrim, peerNsFrag, peerNsSample, peerOverdraw, peerNsDraw;
        for (size_t i = 0; i < reports.size(); ++i)
        {
            const Metrics& m = reports[i].metrics;
            if (!m.queryValid || m.gpuMs < th.minGpuMs) continue;
            if (m.nsPerPrimitive > 0) peerNsPrim.push_back(m.nsPerPrimitive);
            if (m.nsPerFragment > 0) peerNsFrag.push_back(m.nsPerFragment);
            if (m.nsPerSample > 0) peerNsSample.push_back(m.nsPerSample);
            if (m.overdraw > 0) peerOverdraw.push_back(m.overdraw);
            if (m.nsPerDraw > 0) peerNsDraw.push_back(m.nsPerDraw);
        }
        const double medNsPrim = median(peerNsPrim), medNsFrag = median(peerNsFrag);
        const double medNsSample = median(peerNsSample), medOverdraw = median(peerOverdraw);
        const double medNsDraw = median(peerNsDraw);

        // 3) Rule evaluation
        for (size_t i = 0; i < reports.size(); ++i)
        {
            Report& rep = reports[i]; const Metrics& m = rep.metrics;
            std::vector<Finding>& out = rep.findings;

            // ---- geometry / vertex pressure ------------------------------------------------
            {
                double ratio = (medNsPrim > 0) ? m.nsPerPrimitive / medNsPrim : 1.0;
                double sizeScore = (m.primitives >= th.minPrimitives) ? 0.5 : 0.0;
                double cost = ramp(ratio, 1.4) * 0.6 + ramp(m.nsPerPrimitive, th.nsPerPrimitiveHeavy) * 0.4;
                double score = std::max(cost * 0.9, sizeScore * ramp(ratio, th.peerRatio) * 0.8);
                if (m.primitives < th.minPrimitives) score = 0.0;   // per-primitive cost is noise
                if (score > 0.2 && m.queryValid && m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "GEOMETRY_PRESSURE";
                    f.title = "vertex fetch / transform bound";
                    f.detail = "P " + formatCount(m.primitives) + ", V " + formatCount(m.vertices) +
                               ", " + formatNumber("%.2f", m.nsPerPrimitive) + " ns/prim = " +
                               formatNumber("%.2fx", ratio) + " peer median";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- duplicated vertices (poor index reuse) ------------------------------------
            {
                double score = ramp(m.vsPerPrimitive, th.vertexRatioHigh, th.vertexRatioHigh * 1.6);
                if (score > 0.2 && m.primitives >= th.minPrimitives && m.samples > 0)
                {
                    Finding f;
                    f.code = "VERTEX_DUPLICATION";
                    f.title = "vertices are not shared (index/cache efficiency)";
                    f.detail = "V/P = " + formatNumber("%.2f", m.vsPerPrimitive) +
                               " (3.0 means every triangle has unique vertices; higher = duplicated "
                               "positions or non-triangle primitives)";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- vertex bound with little shading (tiny triangles) -------------------------
            {
                if (m.nsPerPrimitive > 0 && m.fragPerVertex < 1.0 &&
                    m.vertices >= th.minPrimitives && m.samples > 0)
                {
                    double score = ramp(1.0 / std::max(m.fragPerVertex, 1e-3), 2.0, 8.0) *
                                   ramp(m.nsPerPrimitive, th.nsPerPrimitiveHeavy);
                    if (score > 0.2)
                    {
                        Finding f;
                        f.code = "TINY_TRIANGLES";
                        f.title = "many small / sub-pixel triangles";
                        f.detail = "F/V = " + formatNumber("%.2f", m.fragPerVertex) +
                                   ", " + formatNumber("%.2f", m.nsPerPrimitive) + " ns/prim";
                        f.score = score; out.push_back(f);
                    }
                }
            }
            // ---- fragment shader / texture pressure ----------------------------------------
            {
                double ratio = (medNsFrag > 0) ? m.nsPerFragment / medNsFrag : 1.0;
                double score = ramp(ratio, 1.5) * 0.6 + ramp(m.nsPerFragment, th.nsPerFragmentHeavy) * 0.4;
                if (score > 0.2 && m.fragments >= th.minFragments && m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "FRAGMENT_PRESSURE";
                    f.title = "fragment shader / texture access bound";
                    f.detail = "F " + formatCount(m.fragments) + ", " +
                               formatNumber("%.2f", m.nsPerFragment) + " ns/frag = " +
                               formatNumber("%.2fx", ratio) + " peer median";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- overdraw (screen space) ------------------------------------------------------
            {
                double score = ramp(m.overdraw, th.overdrawHigh, th.overdrawHigh * 2.0);
                if (medOverdraw >= th.overdrawLow)   // only with a meaningful peer baseline
                    score = std::max(score, ramp(m.overdraw / medOverdraw, th.peerRatio) * 0.8);
                if (score > 0.2 && m.fragments >= th.minFragments &&
                    m.overdraw > 0 && m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "OVERDRAW";
                    f.title = "overdraw: the screen is shaded several times";
                    f.detail = "F/viewport = " + formatNumber("%.2f", m.overdraw) + "x (peer " +
                               formatNumber("%.2f", medOverdraw) + "x), F " +
                               formatCount(m.fragments) + " over " + formatCount(m.pixels);
                    f.score = score; out.push_back(f);
                }
            }
            // ---- shaded but rejected by the depth test -----------------------------------------
            {
                double score = ramp(m.wasted, th.wastedHigh, th.wastedHigh * 2.0);
                if (score > 0.2 && m.fragments >= th.minFragments && m.hasSamples &&
                    m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "WASTED_SHADING";
                    f.title = "fragments are shaded and then dropped by the depth test";
                    f.detail = "F/S = " + formatNumber("%.2f", m.wasted) + " (F " +
                               formatCount(m.fragments) + ", S " + formatCount(m.samples) +
                               "): sort front-to-back, enable early-z / a depth pre-pass";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- depth / blend / framebuffer write -------------------------------------------
            {
                double ratio = (medNsSample > 0) ? m.nsPerSample / medNsSample : 1.0;
                double score = ramp(ratio, 1.5) * 0.6 +
                               ((m.wasted > 0 && m.wasted <= th.overdrawLow) ? 0.4 : 0.0);
                if (score > 0.2 && m.samples >= th.minFragments && m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "FILL_RATE";
                    f.title = "depth test / blending / framebuffer write bound";
                    f.detail = "S " + formatCount(m.samples) + ", " +
                               formatNumber("%.2f", m.nsPerSample) + " ns/sample = " +
                               formatNumber("%.2fx", ratio) + " peer median";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- nothing submitted but time is spent -----------------------------------------
            {
                bool littleWork = (m.primitives < th.minPrimitives) && (m.fragments < th.minFragments);
                if (littleWork && m.gpuMs >= th.fixedCostMs)
                {
                    double score = ramp(m.gpuMs, th.fixedCostMs, th.fixedCostMs * 3.0);
                    if (score > 0.2)
                    {
                        Finding f;
                        f.code = "FIXED_COST";
                        f.title = "no significant draw work but GPU time is spent";
                        f.detail = "T " + formatNumber("%.2f", m.gpuMs) + " ms with P " +
                                   formatCount(m.primitives) + " / F " + formatCount(m.fragments) +
                                   ": waiting on textures, shader compile, buffer upload or a sync";
                        f.score = score; out.push_back(f);
                    }
                }
            }
            // ---- per-draw driver overhead ------------------------------------------------------
            {
                double ratio = (medNsDraw > 0) ? m.nsPerDraw / medNsDraw : 1.0;
                double score = std::max(ramp(m.nsPerDraw, th.hugeNsPerDraw),
                                        ramp(ratio, th.peerRatio) * 0.8);
                if (score > 0.2 && m.draws >= 8 && m.gpuMs >= th.minGpuMs)
                {
                    Finding f;
                    f.code = "DRAW_OVERHEAD";
                    f.title = "per-draw cost dominates";
                    f.detail = "T/draw = " + formatNumber("%.1f", m.nsPerDraw) + " ns over " +
                               formatCount(m.draws) + " draws";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- batching opportunity ----------------------------------------------------------
            {
                if (m.draws >= th.manyDrawCalls && m.primitivesPerDraw > 0 &&
                    m.primitivesPerDraw < th.tinyPrimitivesPerDraw)
                {
                    double score = ramp(m.draws, th.manyDrawCalls, th.manyDrawCalls * 4.0);
                    Finding f;
                    f.code = "BATCHING";
                    f.title = "too many small draws";
                    f.detail = formatCount(m.draws) + " draws, " +
                               formatNumber("%.1f", m.primitivesPerDraw) + " prims/draw: merge, "
                               "instance or use a shared state";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- frame spike vs history ---------------------------------------------------------
            {
                if (m.historyMs > th.minGpuMs)
                {
                    double ratio = m.gpuMs / m.historyMs;
                    double score = ramp(ratio, th.spikeRatio, th.spikeRatio * 2.0);
                    if (score > 0.2)
                    {
                        Finding f;
                        f.code = "SPIKE";
                        f.title = "this frame is much slower than recent history";
                        f.detail = formatNumber("%.2f", m.gpuMs) + " ms vs median " +
                                   formatNumber("%.2f", m.historyMs) + " ms (" +
                                   formatNumber("%.1fx", ratio) + "): upload, compile, GC or a "
                                   "one-off resource";
                        f.score = score; out.push_back(f);
                    }
                }
            }
            // ---- resource pressure ---------------------------------------------------------------
            {
                // Only node scopes own the resources they draw: a camera scope would add the same
                // bytes again, and a single scope is trivially 100% of the watched resources, so
                // the "share" branch needs at least two node scopes to mean anything
                double total = 0.0; int scopes = 0;
                for (size_t k = 0; k < reports.size(); ++k)
                {
                    if (reports[k].cameraScope) continue;
                    total += reports[k].metrics.geometryMB + reports[k].metrics.textureMB;
                    ++scopes;
                }
                double mine = m.geometryMB + m.textureMB;
                double share = total > 0 ? mine / total : 0.0;
                double score = std::max(ramp(m.textureMB, th.bigTextureMB, th.bigTextureMB * 2.0),
                                        ramp(m.geometryMB, th.bigGeometryMB, th.bigGeometryMB * 2.0));
                if (score < 0.2 && scopes > 1) score = ramp(share, th.resourceShare) * 0.5;
                score = std::min(score, 0.65);   // memory is a constraint, not a stall
                if (score > 0.2 && mine > 0.0)
                {
                    Finding f;
                    f.code = "RESOURCE_PRESSURE";
                    f.title = "memory footprint";
                    f.detail = "geometry " + formatNumber("%.1f", m.geometryMB) + " MB, textures " +
                               formatNumber("%.1f", m.textureMB) + " MB (exclusive " +
                               formatNumber("%.1f", m.exclusiveMB) + " MB), " +
                               formatNumber("%.0f", share * 100.0) + "% of watched";
                    f.score = score; out.push_back(f);
                }
            }
            // ---- fully occluded / nothing visible --------------------------------------------------
            {
                if (m.fragments > th.minFragments && !m.hasSamples)
                {
                    Finding f;
                    f.code = "FULLY_OCCLUDED";
                    f.title = "fragments are shaded but none passes the depth test";
                    f.detail = "S = 0 with F " + formatCount(m.fragments) +
                               ": geometry is hidden, or the depth func/test is broken";
                    f.score = 0.5; out.push_back(f);
                }
            }

            // 4) Sort and build the verdict
            std::sort(out.begin(), out.end(),
                      [](const Finding& a, const Finding& b) { return a.score > b.score; });
            rep.verdict = out.empty() ? "balanced (no dominant pressure)" : out[0].title;
        }
        return reports;
    }

    void PerfAnalyzer::print(std::ostream& out, const std::vector<Report>& reports, bool details)
    {
        for (size_t i = 0; i < reports.size(); ++i)
        {
            const Report& r = reports[i]; const Metrics& m = r.metrics;
            out << "[Analyze] " << r.name << (r.cameraScope ? " (camera)" : " (node)") << ": "
                << "T " << formatNumber("%.3f", m.gpuMs) << " ms";
            if (m.share > 0) out << " (" << formatNumber("%.0f", m.share * 100.0) << "%)";
            if (m.queryValid)
            {
                out << ", P " << formatCount(m.primitives) << ", V " << formatCount(m.vertices)
                    << ", F " << formatCount(m.fragments) << ", S " << formatCount(m.samples);
                if (m.overdraw > 0)
                    out << ", OD " << formatNumber("%.2f", m.overdraw) << "x, cover "
                        << formatNumber("%.2f", m.coverage) << "x";
                if (m.wasted > 1.05) out << ", F/S " << formatNumber("%.2f", m.wasted);
            }
            else
                out << ", counters n/a (no GL3 query support)";
            out << ", draws " << (r.cameraScope ? formatCount(m.drawables) + " drawables"
                                                : formatCount(m.draws) + " calls");
            out << "\n          verdict: " << r.verdict << "\n";

            if (details)
            {
                for (size_t k = 0; k < r.findings.size(); ++k)
                {
                    const Finding& f = r.findings[k];
                    out << "          - " << f.code << " (" << formatNumber("%.2f", f.score)
                        << ") " << f.title << "\n            " << f.detail << "\n";
                }
            }
        }
    }

    void PerfAnalyzer::print(std::ostream& out, const GlobalReport& report, bool details)
    {
        out << "[Analyze] whole frame: ";
        if (report.frameMs > 0.0) out << "frame " << formatNumber("%.2f", report.frameMs) << " ms, ";
        out << "scopes sum " << formatNumber("%.2f", report.totalGpuMs) << " ms, geometry "
            << formatNumber("%.0f", double(report.geometryBytes) / (1024.0 * 1024.0)) << " MB, textures "
            << formatNumber("%.0f", double(report.textureBytes) / (1024.0 * 1024.0)) << " MB";
        if (report.gpuMemory.supported)
            out << ", VRAM " << formatNumber("%.0f", double(report.gpuMemory.getUsedBytes()) / (1024.0 * 1024.0))
                << "/" << formatNumber("%.0f", double(report.gpuMemory.totalBytes) / (1024.0 * 1024.0)) << " MB";
        else
            out << ", VRAM n/a";
        out << "\n          verdict: " << report.verdict << "\n";

        if (details)
        {
            for (size_t k = 0; k < report.findings.size(); ++k)
            {
                const Finding& f = report.findings[k];
                out << "          - " << f.code << " (" << formatNumber("%.2f", f.score)
                    << ") " << f.title << "\n            " << f.detail << "\n";
            }
        }
    }

    std::string PerfAnalyzer::toString(const PerfProfiler& profiler, bool details,
                                       const Thresholds& th)
    {
        std::ostringstream out; print(out, analyze(profiler, th), details); return out.str();
    }
}
