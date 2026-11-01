# Animate.dll

Native C++ Rainmeter plugin for animated GIF and APNG files. Frames are decoded
once into PNGs; a normal `Meter=Image` displays them. No external codec, command
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
| `Speed` | `0` uses each frame's embedded duration. Positive values override all frame durations in milliseconds, clamped to 10–2147483647 ms. | `0` |
| `Loop` | Additional repeats after the first play-through: `0` plays once, `1` plays twice, `-1` repeats endlessly. Overrides embedded loop counts. | `0` |

The measure's **numeric value is the effective frame delay in milliseconds**.
For a positive Speed it returns that fixed duration. With `Speed=0`, it returns
the current frame's embedded duration, including the documented minimum and
rounding. Pausing retains the current frame's duration; ending reports the
first frame's duration. It returns `0` when no source has loaded successfully.
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
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "start"]
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "pause"]
LeftMouseUpAction=[!CommandMeasure MeasureAnimate "end"]
```

`start` begins playback, resumes a paused animation at its current frame, or
restarts a completed animation. Repeated `start` while playing has no effect.
`pause` retains the displayed frame and repeat count; resuming waits one full
duration for that frame before advancing, using its embedded delay when Speed=0.
`end` stops playback, resets the repeat count,
and displays frame 0. Natural completion leaves the last frame displayed.
Loading a source initially displays frame 0, but does not start a timer.

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
variable. Finalization wakes and joins the worker before deleting its files.

The worker posts a message to the UI thread to send `!SetOption "MeterName"
ImageName "frame.png"`, `!UpdateMeter`, and `!Redraw`. All Rainmeter API calls
stay on its UI thread. If a skin update blocks the UI, playback timing continues
on the worker; at most one display message is queued, and the Image meter shows
the latest frame when the UI is available. Rendering itself must wait for the
UI thread, so frames may be skipped visually during a long skin update.
Images are decoded once during reload, and Rainmeter loads the extracted PNGs
for display. Large images or very short intervals can increase display work.

Every measure has a unique `%TEMP%\Rainmeter-Animate-{GUID}` folder, where
`%TEMP%` is the Windows temporary directory returned by `GetTempPathW` and
`{GUID}` is a newly generated identifier. Extracted frames are named
`frame-0.png`, `frame-1.png`, `frame-2.png`, and so on. For example:

```text
%TEMP%\Rainmeter-Animate-{GUID}\frame-0.png
```

These are full-canvas PNG frames, including those extracted from APNG sources.
Each plugin measure owns a separate folder, even when measures use the same
source. Changing the source deletes its old frames. Closing, unloading, or refreshing the skin calls
`Finalize`, which stops the worker and deletes its extracted frames and folder.
Failed extractions also clean up their partial output. Only files created by
this instance are deleted; cleanup does not recursively traverse directories.

GIF transparency, offsets, and disposal are composited into full-canvas PNGs.
APNG additionally supports SOURCE/OVER blending, BACKGROUND/PREVIOUS disposal,
and an excluded default poster image. Limits: 256 MiB input, 16 megapixels per
canvas, 10,000 frames, and 1 GiB of cumulative decoded frame pixels. Decoder
errors appear in Rainmeter's log. Meter names and temporary paths containing
quotes, square brackets, or newlines cannot be used in generated bangs.

## Build and install

Place this repository at `C++\PluginAnimate` inside a local
[Rainmeter plugin SDK](https://github.com/rainmeter/rainmeter-plugin-sdk)
checkout. The plugin's C++ source directly includes
`../../API/RainmeterAPI.h`, and the project links the local SDK's
`API\x32\Rainmeter.lib` or `API\x64\Rainmeter.lib`. SDK files are not bundled
in this repository.

Open `PluginAnimate.vcxproj` in Visual Studio with Desktop development with C++
and a Windows 10/11 SDK installed, or run from a Developer PowerShell in this
repository's directory:

```powershell
msbuild PluginAnimate.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild PluginAnimate.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Outputs are `x64\Release\Animate.dll` and `x32\Release\Animate.dll`. Copy the
matching architecture to `%APPDATA%\Rainmeter\Plugins\Animate.dll`.
Copy the entire `examples\Animate` directory into your Rainmeter skins folder,
then refresh Rainmeter's skin list and load `Animate\Animate.ini`.
`Example.ini` is also provided at the repository root as a convenient copy.
The Image meter spans the background's width and fits the selected animation
into a 125-pixel-high area above the controls. `PreserveAspectRatio=1` centers
the image in that area while preserving its proportions for any chosen source.

The example starts with `@Resources\Loading.gif`. To try the included APNG, set
`AnimationFile=#@#BeachBall.apng` under `[Variables]` and refresh the skin.
The public-domain APNG is Holger Will's 100 × 100 pixel bouncing beach ball with
20 frames. Source and licensing are recorded in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The source is licensed under GNU GPL v2; see [LICENSE](LICENSE) and the
third-party notices.

## Verification

Tests use the actual decoder, worker thread, and Windows message loop with a mocked
Rainmeter host boundary. No live Rainmeter installation is modified by tests.

```powershell
python tests\generate_fixtures.py
msbuild tests\Tests.vcxproj /p:Configuration=Release /p:Platform=x64
.\tests\x64\Release\Tests.exe
msbuild tests\Tests.vcxproj /p:Configuration=Release /p:Platform=Win32
.\tests\x32\Release\Tests.exe
```

Run from this project directory. Tests verify decoded pixel values, alpha
blending, disposal, poster exclusion, corrupt-file rejection, timer playback
without measure updates, per-frame durations, fixed/embedded timing switches,
zero/missing delays, playback during a blocked UI, pause/resume, repeat counts, dynamic source/speed/meter
changes, and directory cleanup on replacement and finalization.

## Measured CPU use

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

Raw samples are in [tests/cpu-results.json](tests/cpu-results.json). To repeat
the measurement with the example already loaded and running:

```powershell
.\tests\MeasureCpu.ps1 -SampleSeconds 30
```

The script temporarily pauses and resumes `MeasureAnimate` in the `Animate`
skin, writes `tests/cpu-results.json`, and leaves playback running afterward.
Repeating it replaces the saved raw samples.

References: [Rainmeter C++ SDK](https://docs.rainmeter.net/developers/plugin/cpp/),
[PNG/APNG specification](https://www.w3.org/TR/png/),
[WIC GIF metadata](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-native-image-format-metadata-queries).
