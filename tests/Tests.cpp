#include <Windows.h>
#define LIBRARY_EXPORTS
#include "../../../API/RainmeterAPI.h"
#undef LIBRARY_EXPORTS
#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <wincodec.h>
#include <wrl/client.h>

// Only Rainmeter's host boundary is mocked; the real decoder, timers, message
// window, commands, reload logic, and cleanup run unchanged.
enum { LOG_ERROR = 1, LOG_WARNING = 2 };
std::map<std::wstring, std::wstring> options;
std::vector<std::wstring> commands, errors;
void* RmGetSkin(void* rm) { return rm; }
double RmReadDouble(void* rm, LPCWSTR name, double value) { return RmReadFormula(rm, name, value); }
LPCWSTR __stdcall RmReadString(void*, LPCWSTR key, LPCWSTR fallback, BOOL)
{
    auto found = options.find(key);
    return found == options.end() ? fallback : found->second.c_str();
}
double __stdcall RmReadFormula(void*, LPCWSTR key, double fallback)
{
    auto found = options.find(key);
    return found == options.end() ? fallback : std::stod(found->second);
}
LPCWSTR __stdcall RmPathToAbsolute(void*, LPCWSTR path) { return path; }
void __stdcall RmLog(void*, int, LPCWSTR message) { errors.emplace_back(message); }
void __stdcall RmExecute(void*, LPCWSTR command) { commands.emplace_back(command); }
#include "../PluginAnimate.cpp"

void Assert(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Pump(DWORD milliseconds)
{
    auto until = GetTickCount64() + milliseconds;
    while (GetTickCount64() < until)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(1);
    }
}
template<typename Predicate> void Until(Predicate ready, const char* message, DWORD timeout = 3000)
{
    const auto deadline = GetTickCount64() + timeout;
    while (!ready() && GetTickCount64() < deadline) Pump(5);
    Assert(ready(), message);
}
std::vector<BYTE> ReadPixels(const std::wstring& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    Assert(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)), "Read extracted image");
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> convert;
    factory->CreateFormatConverter(&convert);
    Assert(SUCCEEDED(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "Convert extracted image");
    UINT w, h; frame->GetSize(&w, &h);
    std::vector<BYTE> pixels(size_t(w) * h * 4);
    Assert(SUCCEEDED(convert->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())), "Read pixels");
    return pixels;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (argc == 2)
        {
            Animation sample;
            ExtractAnimation(argv[1], sample);
            std::cout << "Sample decoded: " << sample.frames.size() << " frames\n";
        }
        auto fixture = [](const wchar_t* name) { return std::filesystem::absolute(std::filesystem::path(L"tests/fixtures") / name).wstring(); };
        for (const auto* name : {L"blend.apng", L"poster.png", L"disposal.gif"})
        {
            std::wstring dir;
            {
                Animation animation;
                ExtractAnimation(fixture(name), animation);
                dir = animation.directory;
                Assert(animation.frames.size() == 4, "Frame count / APNG poster exclusion");
                Assert(std::filesystem::path(animation.frames[0]).extension() == L".bmp", "Animation frames use BMP files");
                Assert(animation.delays == (std::wstring(name) == L"disposal.gif" ? std::vector<UINT>{100,250,10,330} : std::vector<UINT>{100,250,10,333}),
                    "GIF centiseconds / APNG fractions and minimum delay");
                Assert(ReadPixels(animation.frames[0]) == std::vector<BYTE>({0,0,255,255,0,0,255,255}), "Initial canvas");
                Assert(ReadPixels(animation.frames[2]) == std::vector<BYTE>({0,0,255,255,0,255,0,255}), "PREVIOUS disposal restores red canvas");
                if (std::wstring(name) != L"disposal.gif")
                {
                    Assert(ReadPixels(animation.frames[1]) == std::vector<BYTE>({128,0,127,255,0,0,255,255}), "APNG partial-alpha OVER");
                    Assert(ReadPixels(animation.frames[3]) == std::vector<BYTE>(8,0), "APNG BACKGROUND disposal and transparent SOURCE");
                }
                else
                    Assert(ReadPixels(animation.frames[3]) == std::vector<BYTE>({0,0,255,255,0,0,0,0}), "GIF transparent OVER and BACKGROUND disposal");
            }
            Assert(!std::filesystem::exists(dir), "Extraction destructor removes folder");
        }
        {
            Animation zero, missing;
            ExtractAnimation(fixture(L"denominator-zero.apng"), zero);
            ExtractAnimation(fixture(L"no-delay.gif"), missing);
            Assert(zero.delays[0] == 80, "APNG denominator zero means 100");
            Assert(missing.delays[0] == 100, "Missing GIF delay defaults to 100 ms");
        }
        {
            Animation alpha;
            ExtractAnimation(fixture(L"alpha.apng"), alpha);
            const auto pixels = ReadPixels(alpha.frames[0]);
            Assert(pixels[3] == 0 && pixels[7] == 64 && pixels[11] == 128 && pixels[15] == 255,
                "BMP V5 preserves transparent, partially transparent, and opaque alpha values");
            Assert(pixels[12] == 200 && pixels[13] == 80 && pixels[14] == 40, "Opaque colors stay unchanged");
            Assert(pixels[4] >= 198 && pixels[4] <= 202 && pixels[8] >= 198 && pixels[8] <= 202,
                "Partial-alpha colors survive premultiplication round trips");
            std::ifstream input(alpha.frames[0], std::ios::binary);
            BYTE header[70]{};
            input.read(reinterpret_cast<char*>(header), sizeof(header));
            auto little32 = [&](size_t pos) { return UINT(header[pos]) | UINT(header[pos + 1]) << 8 |
                UINT(header[pos + 2]) << 16 | UINT(header[pos + 3]) << 24; };
            Assert(input && header[0] == 'B' && header[1] == 'M' && little32(14) == 124 && header[28] == 32 &&
                little32(66) == 0xff000000, "Frames have a 32-bit BMP V5 header and explicit alpha mask");
        }
        std::wstring badDir;
        {
            Animation bad;
            bool rejected = false;
            try { ExtractAnimation(fixture(L"bad.apng"), bad); } catch (...) { rejected = true; }
            badDir = bad.directory;
            Assert(rejected, "Reject corrupted PNG");
        }
        Assert(!std::filesystem::exists(badDir), "Failed extraction cleans partial files");
        {
            Animation previewed;
            bool shown = false;
            std::vector<BYTE> original;
            ExtractAnimation(L"examples/Animate/@Resources/Loading.gif", previewed, {}, [&](const Animation& first) {
                Assert(first.frames.size() == 1 && !first.preview.empty(), "Preview is published with only the first frame extracted");
                original = ReadPixels(first.frames[0]);
                const auto annotated = ReadPixels(first.preview);
                Assert(original != annotated && std::equal(original.begin(), original.begin() + 4, annotated.begin()),
                    "Loading text changes only the separate preview and preserves surrounding transparency");
                bool whiteText = false;
                for (size_t pixel = 0; pixel < annotated.size(); pixel += 4)
                    if (annotated[pixel] == 255 && annotated[pixel + 1] == 255 && annotated[pixel + 2] == 255) whiteText = true;
                Assert(whiteText, "Preview contains rendered white loading text");
                shown = true;
            });
            Assert(shown && ReadPixels(previewed.frames[0]) == original, "Real first animation frame is never annotated");
        }
        {
            std::wstring cancelledDir;
            bool cancelled = false;
            {
                Animation partial;
                try { ExtractAnimation(fixture(L"blend.apng"), partial, [&] { return !partial.frames.empty(); }); }
                catch (const AnimationCancelled&) { cancelled = true; }
                cancelledDir = partial.directory;
            }
            Assert(cancelled && !std::filesystem::exists(cancelledDir), "Cancelled extraction removes partial frame output");
        }

        options = {{L"ImageName",fixture(L"blend.apng")},{L"MeterName",L"Image"},{L"Speed",L"20"},{L"Loop",L"0"}};
        void* data = nullptr; double maximum = 0;
        Initialize(&data, &options);
        auto* m = static_cast<Measure*>(data);
        const auto reloadStarted = GetTickCount64();
        options[L"ImageName"] = fixture(L"bad.apng");
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        options[L"ImageName"] = fixture(L"blend.apng");
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"end");
        Assert(GetTickCount64() - reloadStarted < 500 && m->loading && !m->animation, "Reload queues extraction without blocking");
        Assert(Update(data) == 0, "Numeric value is zero while loading");
        Assert(!m->displayLoading, "DisplayLoading defaults to zero");
        Until([&] { return !m->loading; }, "Background extraction completes");
        Reload(data, &options, &maximum); // Refresh the host's maximum after loading.
        Assert(m->animation && maximum == 20 && Update(data) == 20, "Measure returns fixed effective duration");
        Assert(std::wstring(GetString(data)) == fixture(L"blend.apng"), "String value returns full source path");
        auto oldDir = m->animation->directory;
        Assert(!m->playing, "Worker waits until start");
        ExecuteBang(data, L"start");
        Assert(m->playing && m->window, "Start arms timer window");
        Assert(m->interval == 20, "Positive Speed overrides embedded duration");
        Until([&] { return m->index > 0; }, "Timer callback arrives");
        Assert(m->index > 0, "Timer advances without any Update calls");
        ExecuteBang(data, L"pause");
        Assert(Update(data) == 20, "Fixed speed value is independent of current frame");
        const auto paused = m->index.load();
        const auto sent = commands.size();
        Pump(60);
        Assert(!m->playing && m->index == paused && commands.size() == sent, "Pause suspends worker");
        Assert(Update(data) == 20, "Numeric value retains paused frame duration");
        ExecuteBang(data, L"PaUsE");
        Pump(40);
        Assert(!m->playing && m->paused && m->index == paused && commands.size() == sent,
            "Repeated Pause is idempotent and does not resume or advance");
        ExecuteBang(data, L"PaUsErEsUmE");
        Assert(m->playing && m->index == paused, "PauseResume resumes without resetting frame");
        ExecuteBang(data, L"pause");
        ExecuteBang(data, L"start");
        Assert(m->playing && m->index == 0 && m->repeats == 0, "Start restarts a paused animation");
        Until([&] { return m->index > 0; }, "Restarted playback advances");
        ExecuteBang(data, L"start");
        Assert(m->playing && m->index == 0 && m->repeats == 0, "Start restarts an already playing animation");
        Until([&] { return m->completed.load(); }, "One play-through completes");
        Assert(!m->playing && m->completed && m->index == 3, "Loop=0 stops on last frame");
        Assert(Update(data) == 20, "Completed playback reports effective speed");
        ExecuteBang(data, L"PauseResume");
        Assert(!m->playing && m->completed, "PauseResume does not restart completed playback");
        ExecuteBang(data, L"start");
        Assert(m->index == 0 && m->playing, "Start after completion restarts");
        ExecuteBang(data, L"end");
        Assert(!m->playing && m->index == 0 && m->repeats == 0, "End stops and resets");
        Assert(Update(data) == 20, "End retains fixed speed value");
        ExecuteBang(data, L"pause");
        Assert(!m->playing && !m->paused, "Pause does not start an ended animation");
        ExecuteBang(data, L"PauseResume");
        Assert(!m->playing && !m->paused, "PauseResume does not start an ended animation");
        options[L"Speed"] = L"30000";
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        Sleep(30); // Let the worker enter its 30-second interruptible wait.
        const auto endStarted = GetTickCount64();
        ExecuteBang(data, L"end");
        Assert(GetTickCount64() - endStarted < 500, "End immediately interrupts a long frame wait");
        Assert(!m->playing && !m->paused && m->index == 0 && m->repeats == 0, "End clears every playback state");
        options[L"Speed"] = L"20";
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        const auto queuedDeadline = GetTickCount64() + 3000;
        while (m->index == 0 && GetTickCount64() < queuedDeadline) Sleep(5);
        Assert(m->index > 0, "Worker has queued an undispatched frame");
        ExecuteBang(data, L"end");
        const auto afterEndCommands = commands.size();
        Pump(60);
        Assert(!m->playing && m->index == 0 && commands.size() == afterEndCommands,
            "End invalidates pending display updates and cannot advance afterward");
        options.erase(L"Speed");
        Reload(data, &options, &maximum);
        Assert(m->speed == 0, "Omitted Speed defaults to embedded timing");
        Assert(Update(data) == 100 && maximum == 333, "Automatic value reports first frame duration and largest duration as maximum");
        ExecuteBang(data, L"start");
        Assert(m->interval == 100, "Speed=0 schedules first embedded duration");
        Until([&] {
            std::lock_guard<std::mutex> lock(m->mutex);
            return m->index == 1 && m->interval == 250;
        }, "Automatic playback advances with its next duration");
        Assert(m->interval == 250, "Automatic playback schedules next frame's distinct duration");
        ExecuteBang(data, L"pause");
        Assert(Update(data) == 250, "Automatic value reports paused frame's embedded duration");
        ExecuteBang(data, L"PauseResume");
        Assert(m->playing && m->index == 1 && m->interval == 250, "Automatic PauseResume retains frame and duration");
        options[L"Speed"] = L"30";
        Reload(data, &options, &maximum);
        Assert(m->playing && m->index == 1 && m->interval == 30, "Dynamic switch to fixed duration");
        Assert(Update(data) == 30 && maximum == 30, "Dynamic fixed speed changes numeric value and maximum");
        options[L"Speed"] = L"0";
        Reload(data, &options, &maximum);
        Assert(m->playing && m->index == 1 && m->interval == 250, "Dynamic switch to embedded duration");
        Until([&] { return m->completed.load(); }, "Embedded playback completes after last frame duration");
        ExecuteBang(data, L"start");
        const auto beforeBlockedUi = commands.size();
        // Model a busy skin update without dispatching any UI messages. Allow
        // scheduling jitter rather than assuming a fixed wall-clock sleep.
        const auto blockedDeadline = GetTickCount64() + 3000;
        while (m->index != 3 && GetTickCount64() < blockedDeadline) Sleep(5);
        Assert(m->index == 3 && commands.size() == beforeBlockedUi, "Worker progresses independently while UI is blocked");
        Pump(5);
        Assert(commands.size() > beforeBlockedUi && commands.back().find(L"frame-3.bmp") != std::wstring::npos,
            "UI receives latest frame without replaying a backlog");
        ExecuteBang(data, L"end");
        options[L"Speed"] = L"20";
        options[L"Loop"] = L"1";
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        Until([&] { return m->completed.load(); }, "Finite repeat completes");
        Assert(m->completed && m->repeats == 1, "Positive loop gives additional repeat");
        options[L"Loop"] = L"-1";
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        Until([&] { return m->repeats >= 1; }, "Endless playback repeats");
        Assert(m->playing && m->repeats >= 1, "Endless loop");
        ExecuteBang(data, L"pause");
        const auto heldRepeats = m->repeats.load();
        const auto heldFrame = m->index.load();
        ExecuteBang(data, L"PauseResume");
        Assert(m->playing && m->repeats == heldRepeats && m->index == heldFrame, "PauseResume preserves completed repeats");
        options[L"ImageName"] = fixture(L"disposal.gif");
        options[L"Speed"] = L"50";
        options[L"MeterName"] = L"OtherImage";
        Reload(data, &options, &maximum);
        Assert(m->loading && m->startWhenReady && m->speed == 50, "Dynamic source/speed queues background load and preserves requested running state");
        Until([&] { return !m->loading; }, "Replacement source loads");
        Assert(m->animation && m->playing, "Replacement resumes requested playback");
        Assert(!std::filesystem::exists(oldDir), "Source replacement removes old folder");
        Assert(std::wstring(GetString(data)) == fixture(L"disposal.gif"), "Dynamic source changes string value");
        Assert(commands.back().find(L"[!UpdateMeter \"OtherImage\"]") != std::wstring::npos, "Targets normal image meter");
        auto finalDir = m->animation->directory;
        std::weak_ptr<AsyncLoader> cleanup = m->loader;
        Finalize(data);
        Until([&] { return cleanup.expired(); }, "Background cleanup completes");
        Assert(!std::filesystem::exists(finalDir), "Finalize removes temporary folder");
        Pump(60); // Pending timer messages after finalization must be harmless.
        Assert(errors.empty(), "No unexpected plugin errors");
        if (argc == 3 && std::wstring(argv[1]) == L"--large")
        {
            options = {{L"ImageName",argv[2]},{L"MeterName",L"LargeImage"},{L"Loop",L"-1"},{L"DisplayLoading",L"1"}};
            Initialize(&data, &options);
            m = static_cast<Measure*>(data);
            const auto began = GetTickCount64();
            const auto beganPrecise = std::chrono::steady_clock::now();
            Reload(data, &options, &maximum);
            const auto reloadMs = GetTickCount64() - began;
            const auto reloadPreciseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beganPrecise).count();
            Assert(reloadMs < 500 && m->loading, "Large GIF Reload returns immediately");
            ExecuteBang(data, L"start");
            ExecuteBang(data, L"pause");
            Assert(m->paused && !m->startWhenReady, "Pause during loading cancels deferred start");
            ExecuteBang(data, L"pause");
            Assert(m->paused && !m->startWhenReady, "Repeated Pause during loading cannot resume deferred playback");
            ExecuteBang(data, L"PauseResume");
            Assert(m->startWhenReady, "PauseResume during loading resumes deferred start");
            ExecuteBang(data, L"PauseResume");
            Assert(m->paused && !m->startWhenReady, "PauseResume during loading toggles back to paused");
            ExecuteBang(data, L"PauseResume");
            ExecuteBang(data, L"end");
            Assert(!m->startWhenReady && !m->paused, "End during loading cancels deferred playback");
            ExecuteBang(data, L"start");
            size_t heartbeats = 0;
            ULONGLONG maxUiMs = reloadMs;
            double maxPreciseUiMs = reloadPreciseMs;
            double previewMs = -1;
            std::wstring previewPath;
            while (m->loading && GetTickCount64() - began < 120000)
            {
                const auto call = GetTickCount64();
                const auto callPrecise = std::chrono::steady_clock::now();
                Update(data);
                GetString(data);
                maxUiMs = (std::max)(maxUiMs, GetTickCount64() - call);
                maxPreciseUiMs = (std::max)(maxPreciseUiMs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - callPrecise).count());
                ++heartbeats;
                Pump(5);
                if (previewMs < 0 && !m->preview.empty())
                {
                    previewMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beganPrecise).count();
                    previewPath = m->preview;
                    Assert(std::filesystem::path(previewPath).filename() == L"loading.png", "Early frame is the separate loading preview");
                    const auto copy = sizeof(void*) == 8 ? L"tests/x64/Release/loading-preview.png" : L"tests/x32/Release/loading-preview.png";
                    Assert(CopyFileW(previewPath.c_str(), copy, FALSE) != FALSE, "Capture loading preview for visual verification");
                    Assert(commands.back().find(L"loading.png") != std::wstring::npos, "DisplayLoading=1 displays the annotated preview");
                    const auto request = m->loadRevision;
                    options[L"DisplayLoading"] = L"0";
                    Reload(data, &options, &maximum);
                    Assert(m->loadRevision == request && m->loading && commands.back().find(L"frame-0.bmp") != std::wstring::npos,
                        "DisplayLoading=0 switches to the plain first frame without restarting extraction");
                    options[L"DisplayLoading"] = L"1";
                    Reload(data, &options, &maximum);
                    Assert(m->loadRevision == request && commands.back().find(L"loading.png") != std::wstring::npos,
                        "DisplayLoading can be re-enabled during extraction");
                }
            }
            Assert(!m->loading && m->animation && m->playing, "Large GIF completes and honors deferred Start");
            Assert(heartbeats > 0 && maxUiMs < 500, "UI remains responsive throughout large extraction");
            Assert(previewMs >= 0 && previewMs < GetTickCount64() - began, "Annotated preview arrives before extraction completes");
            Until([&] { return !std::filesystem::exists(previewPath); }, "Loading preview is deleted after switching to real frames");
            std::cout << "Large GIF: " << m->animation->frames.size() << " frames; load " << GetTickCount64() - began
                << " ms; preview " << previewMs << " ms; Reload " << reloadPreciseMs << " ms; max UI call " << maxPreciseUiMs << " ms; " << heartbeats << " UI heartbeats\n";
            ExecuteBang(data, L"end");
            // Start another large extraction, then supersede it with a small source.
            options[L"ImageName"] = L"";
            Reload(data, &options, &maximum);
            options[L"ImageName"] = argv[2];
            Reload(data, &options, &maximum);
            ExecuteBang(data, L"start");
            Sleep(30);
            options[L"ImageName"] = fixture(L"disposal.gif");
            const auto switchBegan = GetTickCount64();
            Reload(data, &options, &maximum);
            Assert(GetTickCount64() - switchBegan < 500, "Source replacement does not wait for old extraction or deletion");
            ExecuteBang(data, L"end");
            Until([&] { return !m->loading; }, "Cancelled source is replaced", 30000);
            Assert(m->animation && m->animation->frames.size() == 4 && !m->playing,
                "Latest source wins and End prevents deferred autoplay");
            options[L"ImageName"] = argv[2];
            Reload(data, &options, &maximum);
            Sleep(30);
            cleanup = m->loader;
            const auto closeBegan = GetTickCount64();
            Finalize(data);
            const auto closeMs = GetTickCount64() - closeBegan;
            Assert(closeMs < 500, "Finalize does not wait for extraction or cleanup");
            Until([&] { return cleanup.expired(); }, "Cancelled load cleans up after unload", 30000);
            Assert(errors.empty(), "No unexpected large-file errors");
            std::cout << "Large GIF: source-change cancellation and unload cleanup passed; Finalize " << closeMs << " ms\n";
        }
        CoUninitialize();
        std::cout << "PASS: GIF/APNG pixels, transparency, disposal, poster exclusion, corruption, timers, pause/resume, loops, reload, cleanup\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
