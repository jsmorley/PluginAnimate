#include <Windows.h>
#include "../../API/RainmeterAPI.h"
#include "Animation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>
#include <memory>
#include <stdexcept>
#include <string>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
constexpr UINT FrameMessage = WM_APP + 1;
struct Measure
{
    void* rm = nullptr;
    void* skin = nullptr;
    HWND window = nullptr;
    UINT_PTR generation = 0;
    UINT speed = 0;
    std::atomic<UINT> interval{0};
    int loop = 0;
    std::atomic<uint64_t> repeats{0};
    std::atomic<size_t> index{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> completed{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    bool shutdown = false;
    bool pending = false;
    std::wstring source, meter;
    std::unique_ptr<Animation> animation;

    void StopPlayback()
    {
        // Called with mutex held. Invalidate any display message already queued.
        ++generation;
        pending = false;
        playing = false;
        condition.notify_all();
    }
    ~Measure()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            StopPlayback();
            shutdown = true;
            condition.notify_all();
        }
        if (worker.joinable()) worker.join();
        if (window) DestroyWindow(window);
        // Animation's destructor removes the extracted files and the private folder.
    }
    void StartPlayback()
    {
        StopPlayback();
        interval = speed ? speed : animation->delays[index];
        playing = true;
        condition.notify_all();
    }
    void Show()
    {
        std::wstring command;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!animation || animation->frames.empty() || meter.empty()) return;
            command = L"[!SetOption \"" + meter + L"\" ImageName \"" + animation->frames[index] +
                L"\"][!UpdateMeter \"" + meter + L"\"][!Redraw]";
        }
        // Execute last: meter actions can refresh/unload this measure during RmExecute.
        RmExecute(skin, command.c_str());
    }
    void Run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!shutdown)
        {
            condition.wait(lock, [&] { return shutdown || playing; });
            if (shutdown) break;
            const auto runGeneration = generation;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval.load());
            while (!shutdown && playing && runGeneration == generation)
            {
                if (condition.wait_until(lock, deadline, [&] { return shutdown || !playing || runGeneration != generation; })) break;
                if (index + 1 < animation->frames.size()) ++index;
                else if (loop == -1 || repeats < static_cast<uint64_t>(loop)) { ++repeats; index = 0; }
                else { playing = false; completed = true; break; }
                // Coalesce display work: the UI presents the latest frame after a busy update.
                if (!pending)
                    pending = PostMessageW(window, FrameMessage, runGeneration, 0) != FALSE;
                interval = speed ? speed : animation->delays[index];
                // Anchor deadlines to playback time, not skin updates or rendering time.
                deadline += std::chrono::milliseconds(interval.load());
            }
        }
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto measure = reinterpret_cast<Measure*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == FrameMessage && measure)
    {
        bool show = false;
        {
            std::lock_guard<std::mutex> lock(measure->mutex);
            if (wParam == measure->generation && measure->pending)
            {
                measure->pending = false;
                show = true;
            }
        }
        if (show) measure->Show();
        return 0;
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wParam, lParam);
}

// Each instance owns its window class, avoiding a registered callback into an unloaded DLL.
std::wstring ClassName(void* measure) { return L"RainmeterAnimate_" + std::to_wstring(reinterpret_cast<UINT_PTR>(measure)); }
HINSTANCE Module()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&WindowProc), &module);
    return module;
}
bool SafeArgument(const std::wstring& text)
{
    return text.find_first_of(L"\"\r\n[]") == std::wstring::npos;
}
}

PLUGIN_EXPORT void Initialize(void** data, void* rm)
{
    auto measure = std::make_unique<Measure>();
    measure->rm = rm;
    measure->skin = RmGetSkin(rm);
    const auto name = ClassName(measure.get());
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = Module();
    wc.lpszClassName = name.c_str();
    if (RegisterClassW(&wc))
        measure->window = CreateWindowExW(0, name.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, measure.get());
    if (!measure->window) RmLog(rm, LOG_ERROR, L"Animate: Cannot create display message window.");
    else
    {
        try { measure->worker = std::thread(&Measure::Run, measure.get()); }
        catch (const std::exception&)
        {
            RmLog(rm, LOG_ERROR, L"Animate: Cannot create playback thread.");
        }
    }
    *data = measure.release();
}

PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue)
{
    auto& m = *static_cast<Measure*>(data);
    std::unique_lock<std::mutex> lock(m.mutex);
    bool show = false;
    std::wstring raw = RmReadString(rm, L"ImageName", L"");
    std::wstring source = raw.empty() ? L"" : RmPathToAbsolute(rm, raw.c_str());
    std::wstring meter = RmReadString(rm, L"MeterName", L"");
    double speedValue = RmReadDouble(rm, L"Speed", 0);
    double loopValue = RmReadDouble(rm, L"Loop", 0);
    UINT speed = speedValue == 0.0 ? 0 : (std::isfinite(speedValue) ? static_cast<UINT>((std::clamp)(speedValue, double(USER_TIMER_MINIMUM), double(USER_TIMER_MAXIMUM))) : 0);
    int loop = std::isfinite(loopValue) ? static_cast<int>((std::clamp)(loopValue, -1.0, double(INT_MAX))) : 0;
    if (!SafeArgument(meter))
    {
        RmLog(rm, LOG_ERROR, L"Animate: MeterName cannot contain quotes, brackets, or newlines.");
        meter.clear();
    }
    const bool wasPlaying = m.playing;
    const bool sourceChanged = source != m.source;
    const bool speedChanged = speed != m.speed;
    show = meter != m.meter;
    m.meter = std::move(meter); m.speed = speed; m.loop = loop;
    if (sourceChanged)
    {
        m.StopPlayback();
        m.animation.reset(); // Clean old source even if replacement extraction fails.
        m.source = std::move(source);
        m.index = 0; m.repeats = 0; m.completed = false;
    }
    if (!m.source.empty() && sourceChanged)
    {
        try
        {
            auto animation = std::make_unique<Animation>();
            ExtractAnimation(m.source, *animation);
            for (const auto& path : animation->frames)
                if (!SafeArgument(path)) throw std::runtime_error("Temporary path contains unsupported bang delimiters");
            m.animation = std::move(animation);
            show = true;
        }
        catch (const std::exception& error)
        {
            // Decoder errors are ASCII; source and measure are identified by Rainmeter's log context.
            const std::string text = error.what();
            std::wstring message = L"Animate: " + std::wstring(text.begin(), text.end());
            RmLog(rm, LOG_ERROR, message.c_str());
        }
    }
    if (m.meter.empty() || !m.animation) m.StopPlayback();
    else if (wasPlaying && (sourceChanged || speedChanged)) m.StartPlayback();
    *maxValue = m.animation ? double(m.speed ? m.speed : *std::max_element(m.animation->delays.begin(), m.animation->delays.end())) : 0.0;
    lock.unlock();
    if (show) m.Show();
}

PLUGIN_EXPORT double Update(void* data)
{
    auto& m = *static_cast<Measure*>(data);
    std::lock_guard<std::mutex> lock(m.mutex);
    return m.animation ? double(m.speed ? m.speed : m.animation->delays[m.index]) : 0.0;
}

PLUGIN_EXPORT LPCWSTR GetString(void* data)
{
    // Source changes only on the UI thread in Reload; the worker never modifies it.
    return static_cast<Measure*>(data)->source.c_str();
}

PLUGIN_EXPORT void ExecuteBang(void* data, LPCWSTR args)
{
    auto& m = *static_cast<Measure*>(data);
    std::unique_lock<std::mutex> lock(m.mutex);
    if (_wcsicmp(args, L"start") == 0)
    {
        if (!m.worker.joinable() || !m.animation || m.meter.empty() || m.playing) return;
        if (m.completed) { m.index = 0; m.repeats = 0; m.completed = false; }
        m.StartPlayback();
        lock.unlock();
        m.Show();
    }
    else if (_wcsicmp(args, L"pause") == 0) m.StopPlayback();
    else if (_wcsicmp(args, L"end") == 0)
    {
        m.StopPlayback(); m.index = 0; m.repeats = 0; m.completed = false;
        lock.unlock();
        m.Show();
    }
    else RmLog(m.rm, LOG_WARNING, L"Animate: Unknown command. Use start, pause, or end.");
}

PLUGIN_EXPORT void Finalize(void* data)
{
    auto measure = static_cast<Measure*>(data);
    const auto name = ClassName(measure);
    delete measure;
    UnregisterClassW(name.c_str(), Module());
}
