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
template<typename Predicate> void Until(Predicate ready, const char* message)
{
    const auto deadline = GetTickCount64() + 3000;
    while (!ready() && GetTickCount64() < deadline) Pump(5);
    Assert(ready(), message);
}
std::vector<BYTE> ReadPixels(const std::wstring& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    Assert(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)), "Read extracted PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> convert;
    factory->CreateFormatConverter(&convert);
    Assert(SUCCEEDED(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "Convert extracted PNG");
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
        std::wstring badDir;
        {
            Animation bad;
            bool rejected = false;
            try { ExtractAnimation(fixture(L"bad.apng"), bad); } catch (...) { rejected = true; }
            badDir = bad.directory;
            Assert(rejected, "Reject corrupted PNG");
        }
        Assert(!std::filesystem::exists(badDir), "Failed extraction cleans partial files");

        options = {{L"ImageName",fixture(L"blend.apng")},{L"MeterName",L"Image"},{L"Speed",L"20"},{L"Loop",L"0"}};
        void* data = nullptr; double maximum = 0;
        Initialize(&data, &options);
        auto* m = static_cast<Measure*>(data);
        Reload(data, &options, &maximum);
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
        Assert(m->playing && m->index == paused, "Pause toggles resume without resetting frame");
        ExecuteBang(data, L"pause");
        ExecuteBang(data, L"start");
        Assert(m->playing && m->index == 0 && m->repeats == 0, "Start restarts a paused animation");
        Until([&] { return m->index > 0; }, "Restarted playback advances");
        ExecuteBang(data, L"start");
        Assert(m->playing && m->index == 0 && m->repeats == 0, "Start restarts an already playing animation");
        Until([&] { return m->completed.load(); }, "One play-through completes");
        Assert(!m->playing && m->completed && m->index == 3, "Loop=0 stops on last frame");
        Assert(Update(data) == 20, "Completed playback reports effective speed");
        ExecuteBang(data, L"start");
        Assert(m->index == 0 && m->playing, "Start after completion restarts");
        ExecuteBang(data, L"end");
        Assert(!m->playing && m->index == 0 && m->repeats == 0, "End stops and resets");
        Assert(Update(data) == 20, "End retains fixed speed value");
        ExecuteBang(data, L"pause");
        Assert(!m->playing && !m->paused, "Pause does not start an ended animation");
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
        ExecuteBang(data, L"pause");
        Assert(m->index == 1 && m->interval == 250, "Automatic pause/resume retains frame and duration");
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
        Assert(commands.size() > beforeBlockedUi && commands.back().find(L"frame-3.png") != std::wstring::npos,
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
        ExecuteBang(data, L"pause");
        Assert(m->playing && m->repeats == heldRepeats && m->index == heldFrame, "Pause toggle preserves completed repeats");
        options[L"ImageName"] = fixture(L"disposal.gif");
        options[L"Speed"] = L"50";
        options[L"MeterName"] = L"OtherImage";
        Reload(data, &options, &maximum);
        Assert(m->playing && m->index == 0 && m->speed == 50, "Dynamic source/speed preserves running state");
        Assert(!std::filesystem::exists(oldDir), "Source replacement removes old folder");
        Assert(std::wstring(GetString(data)) == fixture(L"disposal.gif"), "Dynamic source changes string value");
        Assert(commands.back().find(L"[!UpdateMeter \"OtherImage\"]") != std::wstring::npos, "Targets normal image meter");
        auto finalDir = m->animation->directory;
        Finalize(data);
        Assert(!std::filesystem::exists(finalDir), "Finalize removes temporary folder");
        Pump(60); // Pending timer messages after finalization must be harmless.
        Assert(errors.empty(), "No unexpected plugin errors");
        CoUninitialize();
        std::cout << "PASS: GIF/APNG pixels, transparency, disposal, poster exclusion, corruption, timers, pause/resume, loops, reload, cleanup\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
