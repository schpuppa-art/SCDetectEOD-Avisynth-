///////////////////////////////////////////////////////////////////////////////
//  SCDetectEOD.cpp — scene-change detector for AviSynth+
//
//  Stateless scene-change detector optimized for scanned animation film.
//  Does NOT use motion estimation (unlike MVTools SCDetect) — relies purely
//  on distribution-shape changes in luma and chroma histograms.
//
//  Design goals:
//    - Survive global film-grain flicker (mean Y shifts without shape change)
//    - Catch hard cuts missed by motion-based detectors on animation
//    - Reject false positives caused by large intra-scene object motion
//    - Enforce a minimum scene length to suppress rapid-fire spurious cuts
//
//  Input:  YV12 only.
//  Output: same frame, passthrough; writes frame props.
//
//  Props written:
//    _scd_boundary   (int 0/1)  — 1 if this frame is the first of a new scene
//    _scd_dHistY     (int)      — L1 distance of Y histograms between n and n-1
//    _scd_dHistUV    (int)      — L1 distance of UV 2D histograms between n and n-1
//    _scd_dMeanY     (int)      — |meanY[n] - meanY[n-1]|
//    _scd_isFlicker  (int 0/1)  — mean shift without shape shift (diagnostic)
//    _scd_meanY      (int)      — mean Y of current frame (diagnostic)
//
//  Build: cl /LD /EHsc /O2 SCDetectEOD.cpp /link /OUT:SCDetectEOD.dll
//
//  MSVC is required — AviSynth+ plugins are ABI-incompatible with MinGW/GCC.
///////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include "avisynth.h"

// Per-frame statistics used to compute diffs against neighbors.
struct FrameStats {
    int hY[256];     // Y histogram
    int hUV[256];    // Chroma 2D histogram: 16×16, idx = (U>>4)*16 + (V>>4)
    int meanY;       // Average Y
    int npixY;       // Y plane pixel count (for normalization)
};

class SCDetectEOD : public GenericVideoFilter {
    // Detection thresholds (all tunable via script parameters)
    int th_histY;        // min dHistY for a cut
    int th_histUV;       // min dHistUV for a cut
    int th_flicker_mean; // above this, possible flicker candidate
    int th_flicker_hist; // dHistY below this + big dMeanY = flicker, NOT a cut
    int min_scene;       // post-filter: min frames between two boundaries
    bool debug;

    // Caches. Forward-sweep computation order ensures that min_scene
    // suppression always sees finalized results for previous frames.
    std::unordered_map<int, FrameStats> stats_cache;
    std::unordered_map<int, int>        boundary_cache;
    int last_boundary_computed;

    // ─── Per-frame statistics ────────────────────────────────────────────

    FrameStats compute_stats(int n, IScriptEnvironment* env) {
        auto it = stats_cache.find(n);
        if (it != stats_cache.end()) return it->second;

        // Bound cache growth — on overflow just clear
        if (stats_cache.size() > 512) stats_cache.clear();

        int nc = n < 0 ? 0 : (n >= vi.num_frames ? vi.num_frames - 1 : n);
        PVideoFrame f = child->GetFrame(nc, env);

        const BYTE* yp = f->GetReadPtr(PLANAR_Y);
        const BYTE* up = f->GetReadPtr(PLANAR_U);
        const BYTE* vp = f->GetReadPtr(PLANAR_V);
        const int yPitch = f->GetPitch(PLANAR_Y);
        const int uPitch = f->GetPitch(PLANAR_U);
        const int vPitch = f->GetPitch(PLANAR_V);
        const int w  = vi.width;
        const int h  = vi.height;
        const int uw = w / 2;
        const int uh = h / 2;

        FrameStats s;
        std::memset(s.hY,  0, sizeof(s.hY));
        std::memset(s.hUV, 0, sizeof(s.hUV));
        s.npixY = w * h;

        // Y histogram + mean
        int64_t sumY = 0;
        for (int y = 0; y < h; y++) {
            const BYTE* row = yp + (int64_t)y * yPitch;
            for (int x = 0; x < w; x++) {
                s.hY[row[x]]++;
                sumY += row[x];
            }
        }
        s.meanY = (int)(sumY / s.npixY);

        // UV 2D histogram (16×16 bins, index = (U>>4)*16 + (V>>4))
        for (int y = 0; y < uh; y++) {
            const BYTE* ur = up + (int64_t)y * uPitch;
            const BYTE* vr = vp + (int64_t)y * vPitch;
            for (int x = 0; x < uw; x++) {
                int u4 = ur[x] >> 4;
                int v4 = vr[x] >> 4;
                s.hUV[u4 * 16 + v4]++;
            }
        }

        stats_cache[n] = s;
        return s;
    }

    // ─── Diff helpers ────────────────────────────────────────────────────

    // L1 distance between two histograms, normalized ×1000 / total.
    // Returns 0..2000 (max is 2*total when histograms are disjoint).
    static int hist_L1(const int* a, const int* b, int bins, int total) {
        if (total <= 0) return 0;
        int64_t sum = 0;
        for (int i = 0; i < bins; i++) {
            int d = a[i] - b[i];
            sum += (d < 0 ? -d : d);
        }
        return (int)(sum * 1000 / total);
    }

    // ─── Cut decision ────────────────────────────────────────────────────

    // Raw rule: does the (n-1 → n) transition meet the cut criteria by
    // itself, ignoring temporal context? Used twice per frame for
    // rising-edge detection.
    bool is_raw_cut(int n, IScriptEnvironment* env) {
        if (n <= 0) return false;
        FrameStats cur  = compute_stats(n, env);
        FrameStats prev = compute_stats(n - 1, env);
        int dHistY  = hist_L1(cur.hY,  prev.hY,  256, cur.npixY);
        int dHistUV = hist_L1(cur.hUV, prev.hUV, 256, cur.npixY / 4);
        int dMeanY  = cur.meanY - prev.meanY;
        if (dMeanY < 0) dMeanY = -dMeanY;
        // Flicker: big mean shift with small shape shift → NOT a cut
        if (dMeanY > th_flicker_mean && dHistY < th_flicker_hist) return false;
        return (dHistY >= th_histY && dHistUV >= th_histUV);
    }

    // Final boundary result at frame n, with:
    //   1. Rising-edge detection   (boundary@n = raw_cut@n AND NOT raw_cut@n-1)
    //      — suppresses fade/dissolve spray where raw_cut is true on several
    //        consecutive frames; only the first is marked.
    //   2. Min-scene-length filter — suppresses a cut if another cut was
    //        already marked within the last min_scene frames. By definition
    //        a scene can't be shorter than min_scene frames.
    //
    // Computed via forward sweep so the min_scene window check always sees
    // finalized results for all previous frames. Cached; any subsequent
    // access is O(1).
    int get_boundary(int n, IScriptEnvironment* env) {
        if (n <= 0) return 0;
        auto it = boundary_cache.find(n);
        if (it != boundary_cache.end()) return it->second;

        int from = last_boundary_computed + 1;
        if (from < 1)  from = 1;
        if (from > n)  from = 1;  // restart on gap (e.g. seek backwards)

        for (int k = from; k <= n; k++) {
            if (boundary_cache.count(k)) continue;

            bool cur_cut  = is_raw_cut(k, env);
            bool prev_cut = is_raw_cut(k - 1, env);
            int rising = (cur_cut && !prev_cut) ? 1 : 0;

            if (rising && min_scene > 0) {
                int lo = k - min_scene;
                if (lo < 1) lo = 1;
                for (int j = k - 1; j >= lo; j--) {
                    auto jit = boundary_cache.find(j);
                    if (jit != boundary_cache.end() && jit->second == 1) {
                        rising = 0;
                        break;
                    }
                }
            }

            boundary_cache[k] = rising;
            if (k > last_boundary_computed) last_boundary_computed = k;
        }
        return boundary_cache[n];
    }

public:
    SCDetectEOD(PClip c,
                int thY, int thUV, int thFM, int thFH,
                int minScene, bool dbg,
                IScriptEnvironment* env)
        : GenericVideoFilter(c)
        , th_histY(thY), th_histUV(thUV)
        , th_flicker_mean(thFM), th_flicker_hist(thFH)
        , min_scene(minScene)
        , debug(dbg)
        , last_boundary_computed(0)
    {
        if (!vi.IsYV12())
            env->ThrowError("SCDetectEOD: input must be YV12");
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
        PVideoFrame src = child->GetFrame(n, env);

        // Passthrough pixels
        PVideoFrame dst = env->NewVideoFrameP(vi, &src);
        env->BitBlt(dst->GetWritePtr(PLANAR_Y), dst->GetPitch(PLANAR_Y),
                    src->GetReadPtr(PLANAR_Y),  src->GetPitch(PLANAR_Y),
                    src->GetRowSize(PLANAR_Y),  src->GetHeight(PLANAR_Y));
        env->BitBlt(dst->GetWritePtr(PLANAR_U), dst->GetPitch(PLANAR_U),
                    src->GetReadPtr(PLANAR_U),  src->GetPitch(PLANAR_U),
                    src->GetRowSize(PLANAR_U),  src->GetHeight(PLANAR_U));
        env->BitBlt(dst->GetWritePtr(PLANAR_V), dst->GetPitch(PLANAR_V),
                    src->GetReadPtr(PLANAR_V),  src->GetPitch(PLANAR_V),
                    src->GetRowSize(PLANAR_V),  src->GetHeight(PLANAR_V));

        // Compute metrics for props (reuses cache)
        FrameStats cur  = compute_stats(n, env);
        FrameStats prev = (n > 0) ? compute_stats(n - 1, env) : cur;
        int dHistY  = hist_L1(cur.hY,  prev.hY,  256, cur.npixY);
        int dHistUV = hist_L1(cur.hUV, prev.hUV, 256, cur.npixY / 4);
        int dMeanY  = cur.meanY - prev.meanY;
        if (dMeanY < 0) dMeanY = -dMeanY;
        int isFlicker = (dMeanY > th_flicker_mean && dHistY < th_flicker_hist) ? 1 : 0;

        int isBoundary = get_boundary(n, env);

        AVSMap* props = env->getFramePropsRW(dst);
        env->propSetInt(props, "_scd_boundary",  isBoundary,   0);
        env->propSetInt(props, "_scd_dHistY",    dHistY,       0);
        env->propSetInt(props, "_scd_dHistUV",   dHistUV,      0);
        env->propSetInt(props, "_scd_dMeanY",    dMeanY,       0);
        env->propSetInt(props, "_scd_isFlicker", isFlicker,    0);
        env->propSetInt(props, "_scd_meanY",     cur.meanY,    0);

        return dst;
    }

    int __stdcall SetCacheHints(int h, int) override {
        return h == CACHE_GET_MTMODE ? MT_SERIALIZED : 0;
    }

    static AVSValue __cdecl Create(AVSValue args, void*, IScriptEnvironment* env) {
        return new SCDetectEOD(
            args[0].AsClip(),
            args[1].AsInt(300),    // th_histY
            args[2].AsInt(300),    // th_histUV
            args[3].AsInt(5),      // th_flicker_mean
            args[4].AsInt(100),    // th_flicker_hist
            args[5].AsInt(12),     // min_scene
            args[6].AsBool(false), // debug
            env);
    }
};

const AVS_Linkage* AVS_linkage = nullptr;

extern "C" __declspec(dllexport) const char* __stdcall
AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors)
{
    AVS_linkage = vectors;
    env->AddFunction(
        "SCDetectEOD",
        "c[th_histY]i[th_histUV]i[th_flicker_mean]i[th_flicker_hist]i"
        "[min_scene]i[debug]b",
        SCDetectEOD::Create, nullptr);
    return "SCDetectEOD — histogram-based scene-change detector";
}
