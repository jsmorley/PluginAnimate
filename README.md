# Animate.dll

Native C++ Rainmeter plugin for animated GIF and APNG files. Frames are decoded
once into 32-bit BMP V5 files with alpha; a normal `Meter=Image` displays them. No external codec, command
line program, or C++ runtime installation is required.

```ini
[MeasureAnimate]
Measure=Plugin
Plugin=Animate
ImageName=#@#Loading.gif
MeterName=MeterAnimation
Speed=0
Loop=-1
DynamicVariables=1

[MeterAnimation]
Meter=Image
W=125
H=125
PreserveAspectRatio=1
```

Options:

| Option | Meaning | Default |
| --- | --- | --- |
| `ImageName` | Path and filename of GIF or APNG (including APNG with `.png` extension). Relative paths resolve against the skin directory. | Empty |
| `MeterName` | Section name of the existing Image meter in the same skin. | Empty |
| `DisplayLoading` | `0` shows the plain first frame while loading; `1` shows a preview with centered Loading... text. | `0` |
| `Speed` | `0` uses each frame's embedded duration. Positive values override all frame durations in milliseconds, clamped to 10–2147483647 ms. | `0` |
| `Loop` | Additional repeats after the first play-through: `0` plays once, `1` plays twice, `-1` repeats endlessly. Overrides embedded loop counts. | `0` |

The measure's **numeric value is the effective frame delay in milliseconds**.
For a positive Speed it returns that fixed duration. With `Speed=0`, it returns
the current frame's embedded duration, including the documented minimum and
rounding. Pausing retains the current frame's duration; ending reports the
first frame's duration. While loading, it reports the preview frame's delay
once available, or `0` before the preview exists or if loading fails.
The maximum value is the fixed duration when overridden, or the largest embedded
duration in the loaded animation when using source timing.

The measure's **string value is the full path and filename of the source GIF or
APNG**, resolved against the skin directory for relative paths. It does not
return the extracted temporary frame path. Dynamic source changes update this
string value. Rainmeter samples the numeric value on measure updates; animation
playback continues independently of that sampling rate.
In a dynamic String meter, `[MeasureAnimate:0]` displays the numeric delay with
no decimal places; `[MeasureAnimate]` displays the source path.

Commands (case insensitive):

```ini
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "Start"]
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "Pause"]
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "End"]
```

`Start` always starts over at frame 0 and resets the repeat count, even when
the animation is already playing or paused.
`Pause` toggles between pausing and resuming. It retains the displayed frame
and repeat count; resuming waits one full
duration for that frame before advancing, using its embedded delay when Speed=0.
`Pause` has no effect before playback starts, after `End`, or after natural
completion; use `Start` to start over in those states.
`End` immediately interrupts playing or paused playback, cancels any pending
frame update, resets the repeat count, and displays frame 0. It wakes the worker
without waiting for the current frame delay, even with a long Speed interval.
Natural completion leaves the last frame displayed.
Loading a source happens asynchronously. As soon as the first frame is decoded,
the Image meter displays it while the remaining frames are extracted.
With `DisplayLoading=1`, it displays a separate `loading.png` preview with **Loading...**
superimposed in fixed 12-pixel, normal-weight text in the preview image, over a compact translucent dark panel
in the center. This annotation is
drawn into the preview PNG by the plugin; no extra String meter is required.
`DisplayLoading=0` is the default and uses the plain first frame,
avoiding a flicker of Loading... on small animations. The option can be changed
dynamically without restarting extraction. The actual animation frames remain
unmodified. When extraction completes, playback uses those frames and the
annotated preview is deleted in the background. `Start` received while loading remembers the request and starts
playback when the frames are ready. `Pause` toggles that pending playback
request, and `End` cancels it; extraction may continue in the background so
the frames remain available for a later `Start`.

## Embedded frame timing (`Speed=0`)

Both formats store a duration for each frame; durations may vary within a file.
With `Speed=0` (the default when omitted), Animate reads these durations once during extraction and schedules
each displayed frame using its own delay. A positive Speed overrides them all.

- **GIF:** delay is stored in hundredths of a second. A value of `8` means
  **80 ms**. [GIF specification](https://giflib.sourceforge.net/gifstandard/GIF89a.html)
- **APNG:** delay is stored as a fraction of a second:
  `1000 × delay_num / delay_den` gives milliseconds.
  [APNG specification](https://www.w3.org/TR/png/)

For APNG, a zero denominator is treated as `100`, as specified by the format.
Fractional milliseconds are rounded to the nearest whole millisecond. A zero
delay in either format is clamped to the minimum playback duration of **10 ms**;
a GIF frame with no delay metadata uses **100 ms**. These bounds avoid a busy
loop. A worker thread sleeps until each frame's deadline using a condition
variable, with no polling. Variable frame delays use the same worker thread.
The last frame's delay also elapses before completion or looping.

The example starts with source timing enabled and provides an **Auto** button
to restore `Speed=0` after using its fixed-speed buttons.

With `DynamicVariables=1`, Rainmeter rereads options on measure updates. After
changing variables or options, use `!UpdateMeasure MeasureAnimate` to apply them
immediately. A new source is extracted and reset to frame 0, retaining whether
playback was running or paused. Changing Speed restarts the current interval;
changing Loop keeps the current position and number of completed repeats.
Changing the source filename or refreshing the skin also allows retrying a
failed extraction or reloading a file replaced in place.

Playback runs on a per-measure worker thread, independent of skin Update.
It advances the playback position using a steady clock and sleeps between
frame deadlines. While paused, stopped, or finished, it waits indefinitely
without polling. Commands and dynamic options wake it through a condition
variable. Finalization wakes and joins this playback worker promptly.

Extraction and temporary-file deletion run on a separate background worker at
below-normal thread priority. `Reload` queues a source request and returns
without reading or decoding the animation. The extraction worker never holds
the playback/UI mutex while decoding or writing frames. Changing sources cancels
the old request; stale results and errors are discarded. Codec operations,
frame processing, image writing, and APNG parsing have cancellation checkpoints.
GIF frame metadata is read on demand, and oversized animations are rejected
before writing their frame files.

Closing or refreshing the skin cancels extraction and queues cleanup without
waiting on the UI thread. The background worker retains the DLL until it has
finished cancelling and deleting temporary files, so unloading the measure is
safe even if a codec operation is still finishing. Only UI messages transfer a
completed extraction or error back to Rainmeter; all Rainmeter API calls remain
on the UI thread. Each measure has at most one extraction in progress.

The worker posts a message to the UI thread to send `!SetOption "MeterName"
ImageName "frame.bmp"`, `!UpdateMeter`, and `!Redraw`. All Rainmeter API calls
stay on its UI thread. If a skin update blocks the UI, playback timing continues
on the worker; at most one display message is queued, and the Image meter shows
the latest frame when the UI is available. Rendering itself must wait for the
UI thread, so frames may be skipped visually during a long skin update.
Images are decoded once in the background, and Rainmeter loads the extracted BMP files
for display. Large images or very short intervals can increase display work.

Every measure has a unique `%TEMP%\Rainmeter-Animate-{GUID}` folder, where
`%TEMP%` is the Windows temporary directory returned by `GetTempPathW` and
`{GUID}` is a newly generated identifier. Extracted frames are named
`frame-0.bmp`, `frame-1.bmp`, `frame-2.bmp`, and so on. For example:

```text
%TEMP%\Rainmeter-Animate-{GUID}\frame-0.bmp
```

`loading.png` is the separate annotated first-frame preview in that same
directory. It is discarded as soon as the full animation is ready.

These are full-canvas, uncompressed 32-bit BMP V5 frames, including those
extracted from APNG sources. The V5 header includes an explicit alpha mask,
preserving transparency. Windows 8 or later is required for WIC BMP V5 alpha
support. The single annotated loading preview remains a PNG.
Each plugin measure owns a separate folder, even when measures use the same
source. Changing the source deletes its old frames. Closing, unloading, or refreshing the skin calls
`Finalize`, which stops playback, cancels extraction, and schedules deletion of
its extracted frames and folder on the background worker. Cleanup finishes
after the current codec operation reaches a cancellation checkpoint; the UI
does not wait for it.
Failed extractions also clean up their partial output. Only files created by
this instance are deleted; cleanup does not recursively traverse directories.

Large-file regression check (November 1, 2026): a 715 × 584 pixel `A.gif` with
235 frames loaded in about **2.5–2.8 seconds**, with its annotated preview
appearing in about **90–110 ms**, in concurrent native x64/x32 regression tests.
Extraction continued in the
background. A separate **20-second BMP test in running Rainmeter** loaded the
animation in about **2.3 seconds** and observed the
preview, all 235 extracted frames, and deletion of the preview after loading.
Rainmeter answered **171 UI responsiveness probes with no timeouts**; the
slowest response was **105.91 ms**. Other skins remained running. These are
observations on the development machine, not timing guarantees. Raw live-test
results are in [tests/live-load-results.json](tests/live-load-results.json).

GIF transparency, offsets, and disposal are composited into full-canvas BMPs.
APNG additionally supports SOURCE/OVER blending, BACKGROUND/PREVIOUS disposal,
and an excluded default poster image. Limits: 256 MiB input, 16 megapixels per
canvas, 10,000 frames, and 1 GiB of cumulative decoded frame pixels. Decoder
errors appear in Rainmeter's log. Meter names and temporary paths containing
quotes, square brackets, or newlines cannot be used in generated bangs.

## Extraction performance

Version 1.2.0 uses uncompressed BMP V5 frames and skips unnecessary alpha
conversion for fully opaque pixels. A standalone comparison using the same
235-frame `A.gif` measured:

| Frame output | Extraction time | Temporary frame files |
| --- | ---: | ---: |
| Previous PNG implementation | 12.0–13.1 seconds | 164 MiB |
| BMP V5 with optimized alpha conversion | 1.6–1.9 seconds | 374 MiB |

This trades temporary disk space for lower encoding CPU use and faster startup.
Decoded pixel checksums matched for every frame. A separate alpha fixture
verified transparency values of 0, 64, 128, and 255. The background-worker
regression tests, run concurrently for both architectures, took about
2.5–2.8 seconds including the preview. Timing depends on system load and storage.
Raw samples are in [tests/extraction-benchmark-results.json](tests/extraction-benchmark-results.json).
`tests/PrepareExtractionBench.py` generates the standalone profiling harness in
the ignored `tests/x64/Bench` directory without changing production source.

## Measured CPU use

These CPU results were recorded with the earlier PNG-frame implementation.
They have not been remeasured with BMP frames.

Measured on October 31, 2026 using 64-bit Rainmeter and the installed Animate
example: one 125 × 125 pixel, eight-frame GIF, `Speed=0` (embedded timing), and
`Loop=-1`. Other active skins remained running. The machine exposed 16 logical
processors.

These measurements used the original locally sourced loading GIF from development.
The publicly distributed example uses a reproducible replacement generated by
`examples/generate_loading.py`; the measurements have not been repeated with that
replacement or with the included APNG.

Four nominal 30-second samples were taken in paused / playing / playing / paused
order, with two seconds to settle before each sample. Windows process CPU time
and process CPU-cycle counters were read at each sample's endpoints. Results
below aggregate both samples for each state using their actual elapsed times.
CPU percentages represent total machine capacity across all 16 logical
processors, rather than a percentage of one core.

| Animate state | Rainmeter average CPU | CPU time per elapsed second | CPU cycles per elapsed second |
| --- | ---: | ---: | ---: |
| Paused | 0.42% | 66.76 ms | 256 million |
| Playing | 0.69% | 110.14 ms | 402 million |
| Estimated added playback cost | **0.27 percentage points** | **43.37 ms** | **146 million** |

The added cost is the playing measurement minus the paused baseline. It includes
the animation worker, Image meter image loading, and skin rendering; it is not
a measurement of the worker thread alone. Other skins and normal system
variation can affect the difference. This measures ongoing playback after
extraction, not initial frame extraction, and is not a performance guarantee for
other files, frame rates, skin configurations, or hardware. CPU-cycle counts
are machine-specific counters and are not converted into elapsed time.
