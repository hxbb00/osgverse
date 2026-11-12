#ifndef OSGVERSE_PERF_ANALYZER_H
#define OSGVERSE_PERF_ANALYZER_H

#include "PerfProfiler.h"
#include <ostream>
#include <string>
#include <vector>

namespace osgVerse
{
    /** PerfAnalyzer turns the raw per-scope numbers of PerfProfiler into a ranked list of
        rendering-pressure findings.

        It never touches GL: it only combines the collected counters into derived metrics and
        compares them with (a) the same scope's recent history and (b) the other watched scopes
        of the same frame, so that "high" does not need a hard-coded per-GPU budget.

        Derived metrics:
          overdraw        F / S      fragment invocations per depth-passing sample
          vsPerPrimitive  V / P      ~3 for indexed triangle lists, higher = duplicated vertices
          nsPerPrimitive  T / P      vertex fetch / transform cost per primitive
          nsPerFragment   T / F      fragment shader + texture cost per invocation
          nsPerSample     T / S      depth test / blend / framebuffer write cost per sample
          nsPerDraw       T / draw   fixed per-draw (state change) cost
          primsPerDraw    P / draw   batching quality

        Typical readings (as a guide, not a rule):
          high P/V + high T                  -> geometry / vertex pressure
          high F + high T                    -> fragment shader, texture access, overdraw
          high S + high T                    -> depth test, blending, framebuffer write
          F/S far above the peers            -> wasted shading, overdraw, late discard
          no counter high but T high         -> stalls, uploads, shader compile, sync
    */
    class PerfAnalyzer
    {
    public:
        struct Thresholds
        {
            double minGpuMs;              // ignore scopes cheaper than this (default 0.05)
            double minPrimitives;         // (2e4)
            double minFragments;          // (2e5)
            double overdrawHigh;          // F/viewport pixels deemed too high (2.0)
            double wastedHigh;            // F/S deemed too high (1.5)
            double overdrawLow;           // F/S deemed "no overdraw" (1.3)
            double vertexRatioHigh;       // V/P above this means duplicated vertices (3.2)
            double nsPerPrimitiveHeavy;   // absolute vertex cost hint (5 ns)
            double nsPerFragmentHeavy;    // absolute fragment cost hint (20 ns)
            double manyDrawCalls;         // batching rule (500)
            double tinyPrimitivesPerDraw; // batching rule (64)
            double spikeRatio;            // T vs history median (1.5)
            double peerRatio;             // metric vs peer median (2.0)
            double resourceShare;         // share of the watched resources (0.5)
            double bigTextureMB;          // absolute texture hint (512 MB)
            double bigGeometryMB;         // absolute geometry hint (256 MB)
            double fixedCostMs;           // "no work but time spent" (0.3 ms)
            double hugeNsPerDraw;         // per-draw driver overhead (20000 ns)

            Thresholds();
        };

        struct Metrics
        {
            Metrics();

            double gpuMs, share;          // share of the summed GPU time of watched scopes
            double historyMs;             // median GPU time over the history window
            double primitives, vertices, fragments, samples;
            double draws, drawables, instances;
            double overdraw;              // F / viewport pixels (screen-space overdraw)
            double wasted;                // F / S (shaded but depth-rejected)
            double coverage;              // S / viewport pixels
            double vsPerPrimitive;        // V / P
            double fragPerVertex;         // F / V
            double nsPerPrimitive, nsPerFragment, nsPerSample, nsPerDraw;
            double primitivesPerDraw;
            double geometryMB, textureMB, exclusiveMB, bufferMB;
            double pixels;                // viewport pixels
            bool queryValid;              // T/P/V/F/S were collected
            bool hasSamples;              // S > 0
        };

        struct Finding
        {
            std::string code;             // short upper-case id, e.g. "OVERDRAW"
            std::string title;            // one-line summary
            std::string detail;           // numbers behind the finding
            double score;                 // 0..1 confidence/severity
        };

        struct Report
        {
            std::string name;
            bool cameraScope;
            Metrics metrics;
            std::string verdict;          // dominant pressure
            std::vector<Finding> findings;  // sorted by score, descending
        };

        static Metrics computeMetrics(const PerfProfiler::Result& result,
                                      const std::vector<PerfProfiler::Sample>& history,
                                      double totalGpuMs);

        /** Analyze every scope currently watched by the profiler. */
        static std::vector<Report> analyze(const PerfProfiler& profiler,
                                           const Thresholds& th = Thresholds());

        static void print(std::ostream& out, const std::vector<Report>& reports, bool details = true);
        static std::string toString(const PerfProfiler& profiler, bool details = true,
                                    const Thresholds& th = Thresholds());
    };
}

#endif
