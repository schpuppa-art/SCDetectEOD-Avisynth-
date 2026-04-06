# SCDetectEOD

Histogram-based scene-change detector for AviSynth+, optimized for scanned
animation and other sources where motion-compensated detectors (MVTools
`MSCDetection`, RIFE built-in SCD, etc.) fail.

Writes per-frame properties. Does not modify pixels. Pairs with downstream
filters that can read frame props (or can be adapted via a thin AVS wrapper
for filters that only understand pixels — see the RIFE integration example
below).

## Why another scene detector?

Motion-compensated SCD struggles on certain material:

- **Shot/reverse-shot dialogue** — same lighting, same background, only the
  face/angle changes. Motion SAD is low, the detector reports no cut, and
  interpolators like RIFE produce ugly morph frames between the two angles.
- **Scanned film grain flicker** — random per-frame luma jitter pumps SAD up
  and produces false positives.
- **Hand-drawn animation** — static cel backgrounds with small localized
  animation confuse motion estimators; they read repeating poses as
  repeating scenes and miss real cuts.
- **Large intra-scene object motion** — a large colored object moving or
  inflating across the frame can look like a cut to naive histogram diffs.

`SCDetectEOD` takes a different approach: pure histogram shape comparison
with flicker rejection and a hard minimum-scene-length post-filter. No
motion estimation, no external dependencies, no tuning per-resolution.

## Requirements

- AviSynth+ (r3770 or later — needs frame props API)
- Input must be YV12. Use `ConvertToYV12()` upstream if necessary.

## Build

**MSVC is required.** MinGW/GCC builds of AviSynth+ plugins are silently
ABI-incompatible (virtual-destructor vtable layout differs between Itanium
and MSVC C++ ABIs) and will crash at load time or on first frame.

From an `x64 Native Tools Command Prompt for VS 2022`:

```
cl /LD /EHsc /O2 /MD /DNDEBUG SCDetectEOD.cpp /link /OUT:SCDetectEOD.dll
```

With explicit include path if `avisynth.h` is not next to the source:

```
cl /LD /EHsc /O2 /MD /DNDEBUG ^
   /I "C:\Program Files (x86)\AviSynth+\FilterSDK\include" ^
   SCDetectEOD.cpp ^
   /link /OUT:SCDetectEOD.dll
```

Drop the resulting `SCDetectEOD.dll` into your `plugins64` folder.

## Usage

Minimal:

```
LoadPlugin("SCDetectEOD.dll")
LWLibavVideoSource("input.mkv")
ConvertToYV12()
SCDetectEOD()
```

With all parameters shown explicitly:

```
SCDetectEOD(th_histY=300, th_histUV=300,
            th_flicker_mean=5, th_flicker_hist=100,
            min_scene=12, debug=false)
```

Noisy sources (scanned film, VHS rips, grainy DVDs) can add random jitter
to the UV histograms that inflates `dHistUV` between otherwise-identical
frames. If you observe instability on such material, apply a mild chroma
denoiser upstream before `SCDetectEOD`:

```
src = LWLibavVideoSource("input.mkv").ConvertToYV12()
src.RemoveGrain(20, 18)   # or any chroma-only denoiser
SCDetectEOD()
```

The detector operates on pixel statistics, so any denoise applied to its
input is fine — the denoised clip is not passed downstream unless you
choose to do so.

### Parameters

| Name | Default | Meaning |
|---|---|---|
| `th_histY` | 300 | Minimum L1 distance between Y histograms for a cut. Higher = fewer cuts. Values are normalized ×1000 / pixel count, so they are resolution-independent. |
| `th_histUV` | 300 | Minimum L1 distance between 16×16 UV histograms for a cut. Both Y and UV must exceed their thresholds simultaneously. |
| `th_flicker_mean` | 5 | Flicker guard: frames where `\|meanY[n] - meanY[n-1]\|` exceeds this are candidates for flicker rejection. |
| `th_flicker_hist` | 100 | Flicker guard: if `dHistY` is below this while `dMeanY` is above `th_flicker_mean`, the transition is classified as global brightness flicker and NOT marked as a cut. |
| `min_scene` | 12 | Minimum allowed gap between two cut boundaries, in frames. Suppresses rapid-fire false positives: fade residuals, flash frames, single-frame extreme intra-scene motion. A scene cannot be shorter than this. |
| `debug` | false | Currently a no-op; reserved. Metric overlay is done in AVS via the props below. |

### Frame properties written

| Property | Type | Meaning |
|---|---|---|
| `_scd_boundary` | int 0/1 | **Main output.** 1 if this frame is the first frame of a new scene. |
| `_scd_dHistY` | int | L1 distance of Y histograms between n and n-1, ×1000 / pixel count. |
| `_scd_dHistUV` | int | L1 distance of UV histograms between n and n-1, ×1000 / chroma pixel count. |
| `_scd_dMeanY` | int | `\|meanY[n] - meanY[n-1]\|`. |
| `_scd_isFlicker` | int 0/1 | Diagnostic: 1 if this transition was classified as flicker and NOT marked as a cut. |
| `_scd_meanY` | int | Mean Y of the current frame. |

## Calibration and debug overlay

Drop this in an `.avs` to watch the detector in AvsPmod:

```
LoadPlugin("SCDetectEOD.dll")

src = LWLibavVideoSource("input.mkv").ConvertToYV12()
sd  = src.SCDetectEOD()

ScriptClip(sd, """
  b  = propGetInt("_scd_boundary")
  hy = propGetInt("_scd_dHistY")
  hu = propGetInt("_scd_dHistUV")
  dm = propGetInt("_scd_dMeanY")
  fl = propGetInt("_scd_isFlicker")
  my = propGetInt("_scd_meanY")

  marker = (b == 1) ? "  *** CUT ***" : ""
  flag   = (fl == 1) ? "  [FLICKER]"  : ""

  Subtitle("f" + String(current_frame) + "  meanY=" + String(my) + marker + flag, y=0,  size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("dHistY="  + String(hy), y=26, size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("dHistUV=" + String(hu), y=50, size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("dMeanY="  + String(dm), y=74, size=20, text_color=$ffff00, halo_color=$000000)
""")
```

Scroll through the clip. On every frame where the detector fires you will
see `*** CUT ***` in the overlay. If you find frames that should have been
cut but weren't, or frames marked as cut that shouldn't be, note the metric
values and tune:

- **Missed cuts**: the dHistY or dHistUV on that frame are below the
  current thresholds. Lower them (e.g. `th_histY=250, th_histUV=250`),
  then rerun to see if new false positives appear.
- **False positives**: one of the metrics is just barely above the
  threshold. Raise it, or increase `min_scene` if they come in clusters
  within a short window.
- **Flicker falsely rejecting real cuts**: lower `th_flicker_hist`
  (tighter flicker definition) or raise `th_flicker_mean`.

The defaults were calibrated on a 64k-frame scanned Soviet cel-animation
feature ("Заколдованный мальчик") with hard cuts, fades, flicker and
extreme intra-scene motion. They may need adjustment for live-action or
other material but the structure of the algorithm is the same.

## Integration with RIFE (practical example)

RIFE's built-in scene change detection reads pixel SAD and fires on its
own `sc_threshold`. It does not currently read frame properties. To use
`SCDetectEOD` as the upstream signal, we encode the boundary into pixels
as a border strip that toggles color between scenes — RIFE then sees the
border transition and fires reliably.

This is a functional replacement for the older MVTools-based
`scdetect.avsi` approach — same border-toggle pattern, same RIFE
invocation, only the detector upstream is swapped:

```
LoadPlugin("SCDetectEOD.dll")
# LoadPlugin(...) RIFE, z_ConvertFormat etc. here if not autoloaded

src = LWLibavVideoSource("input.mkv").ConvertToYV12()
sd  = src.SCDetectEOD()

# Border height in pixels. 16 is plenty for RIFE to see the transition.
SC_BORDER_H = 16

borderBlack = sd.AddBorders(0, SC_BORDER_H, 0, 0, color=$000000)
borderWhite = sd.AddBorders(0, SC_BORDER_H, 0, 0, color=$FFFFFF)

global SC_state = 0

# Read _scd_boundary per frame from sd, flip SC_state on boundaries.
toggled = FrameEvaluate(sd, """
    b = propGetInt("_scd_boundary")
    global SC_state = (b == 1) ? (1 - SC_state) : SC_state
""")

# Pick black- or white-framed variant based on SC_state.
signaled = ConditionalFilter(toggled, borderWhite, borderBlack, "SC_state", "=", "1")

# RIFE pipeline
signaled
z_ConvertFormat(pixel_type="RGBPS", colorspace_op="709:709:709:l=>rgb:709:709:f")
Rife(gpu_thread=1, model=73, sc=true, sc_threshold=0.15, \
     fps_num=50, fps_den=1, uhd=true, skip=true)
z_ConvertFormat(pixel_type="YUV420P10", colorspace_op="rgb:709:709:f=>709:709:709:l")
Prefetch(2)
Crop(0, SC_BORDER_H, -0, -0)
```

Why a toggle and not a single pulse: if the border were white on the cut
frame and black on neighbors, RIFE would see **two** border transitions
(black→white and white→black) and duplicate frames around both, producing
~3 duplicate frames per cut instead of a clean transition. Alternating
the color gives **one** transition per cut, coincident with the actual
content change, yielding ~2 duplicate frames at 50fps (40 ms — invisible
in playback).

There is an open feature request on the RIFE plugin (Asd-g/vs-rife-ncnn)
to add an `sc_clip` parameter reading an external signal clip directly,
which would eliminate the pixel-domain encoding entirely. Once that lands
the integration becomes a two-liner.

## Limitations and notes

- **First frame of the clip is never marked** as a boundary (no previous
  frame to compare against). Downstream filters should treat `n=0` as an
  implicit scene start.
- **MT mode is `MT_SERIALIZED`.** The internal frame-property caches and
  forward-sweep boundary computation require serial access. This is not a
  throughput issue in practice — histogram computation is light compared
  to anything downstream.
- **Only hard cuts are marked as single-frame boundaries.** On fades and
  dissolves the raw rule would fire on several consecutive frames; the
  rising-edge filter keeps only the first, and `min_scene` ensures nothing
  fires again too soon.
- **Minimum reliable scene length is `min_scene`.** If your material
  genuinely has shot changes faster than the default 12 frames (music
  videos, modern action editing), lower `min_scene` or set it to 0.

## License

MIT.

## Credits

Developed by **shurik_pronkin** as part of a Soviet-animation film
restoration pipeline. Debugging and calibration carried out
interactively against hard material ("Заколдованный мальчик", 1955).
