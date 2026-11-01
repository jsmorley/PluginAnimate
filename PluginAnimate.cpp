#include <Windows.h>
#include "../../API/RainmeterAPI.h"
#include "Animation.h"
#include "AsyncLoader.h"
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
constexpr UINT LoadMessage = WM_APP + 2;
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
    bool paused = false;
    bool loading = false;
    bool startWhenReady = false;
    bool displayLoading = false;
    UINT_PTR loadRevision = 0;
    std::shared_ptr<AsyncLoader> loader;
    std::wstring source, meter, preview, originalPreview;
    UINT previewDelay = 0;
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
            if (loader) loader->Stop(std::move(animation));
            condition.notify_all();
        }
        if (worker.joinable()) worker.join();
        if (window) DestroyWindow(window);
        // The extraction worker cancels and cleans up independently; never join it here.
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
            const std::wstring image = animation ? animation->frames[index] : (displayLoading ? preview : originalPreview);
            if (!image.empty() && !meter.empty())
                command = L"[!SetOption \"" + meter + L"\" ImageName \"" + image + L"\"][!UpdateMeter \"" + meter + L"\"]";
            if (command.empty()) return;
            command += L"[!Redraw]";
        }
        // Execute last: meter actions can refresh/unload this measure during RmExecute.
        RmExecute(skin, command.c_str());
    }
    void Loaded(UINT_PTR revision, LPARAM stage)
    {
        std::wstring error;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!loader || revision != loadRevision) return;
            if (stage == 1)
            {
                if (!loader->Preview(revision, preview, originalPreview, previewDelay)) return;
            }
            else if (stage == 0)
            {
                if (!loader->Take(revision, animation, error)) return;
                loading = false;
                preview.clear();
                originalPreview.clear();
                if (animation) loader->DiscardPreview(std::move(animation->preview));
                if (animation && startWhenReady && !meter.empty()) StartPlayback();
                if (!animation) paused = false;
                startWhenReady = false;
            }
        }
        if (!error.empty()) RmLog(rm, LOG_ERROR, error.c_str());
        Show();
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
    if (message == LoadMessage && measure)
    {
        measure->Loaded(wParam, lParam);
        return 0;
    }
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
        try
        {
            measure->loader = AsyncLoader::Create(measure->window, LoadMessage);
            measure->worker = std::thread(&Measure::Run, measure.get());
        }
        catch (const std::exception&)
        {
            RmLog(rm, LOG_ERROR, L"Animate: Cannot create background workers.");
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
    const bool displayLoading = RmReadDouble(rm, L"DisplayLoading", 0) != 0;
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
    show = meter != m.meter || displayLoading != m.displayLoading;
    m.displayLoading = displayLoading;
    m.meter = std::move(meter); m.speed = speed; m.loop = loop;
    if (sourceChanged)
    {
        m.StopPlayback();
        m.startWhenReady = (wasPlaying || m.startWhenReady) && !source.empty();
        m.source = std::move(source);
        m.index = 0; m.repeats = 0; m.completed = false;
        m.preview.clear();
        m.originalPreview.clear();
        if (m.loader) m.loadRevision = m.loader->Request(m.source, std::move(m.animation));
        m.loading = m.loader && !m.source.empty();
    }
    if (m.meter.empty() || !m.animation) m.StopPlayback();
    else if (wasPlaying && speedChanged) m.StartPlayback();
    *maxValue = m.animation ? double(m.speed ? m.speed : *std::max_element(m.animation->delays.begin(), m.animation->delays.end())) : 0.0;
    lock.unlock();
    if (show) m.Show();
}

PLUGIN_EXPORT double Update(void* data)
{
    auto& m = *static_cast<Measure*>(data);
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.animation) return double(m.speed ? m.speed : m.animation->delays[m.index]);
    return m.preview.empty() ? 0.0 : double(m.speed ? m.speed : m.previewDelay);
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
        if (!m.worker.joinable() || m.meter.empty()) return;
        m.index = 0; m.repeats = 0; m.completed = false; m.paused = false;
        if (m.loading) { m.StopPlayback(); m.startWhenReady = true; return; }
        if (!m.animation) return;
        m.StartPlayback();
        lock.unlock();
        m.Show();
    }
    else if (_wcsicmp(args, L"pause") == 0 || _wcsicmp(args, L"pauseresume") == 0)
    {
        const bool toggle = _wcsicmp(args, L"pauseresume") == 0;
        if (m.loading)
        {
            if (m.startWhenReady) { m.startWhenReady = false; m.paused = true; }
            else if (toggle && m.paused) { m.startWhenReady = true; m.paused = false; }
        }
        else if (m.playing)
        {
            m.StopPlayback();
            m.paused = true;
        }
        else if (toggle && m.paused && m.worker.joinable() && m.animation && !m.meter.empty())
        {
            m.paused = false;
            m.StartPlayback();
            lock.unlock();
            m.Show();
        }
    }
    else if (_wcsicmp(args, L"end") == 0)
    {
        m.StopPlayback(); m.index = 0; m.repeats = 0; m.completed = false; m.paused = false;
        m.startWhenReady = false;
        lock.unlock();
        m.Show();
    }
    else RmLog(m.rm, LOG_WARNING, L"Animate: Unknown command. Use start, pause, pauseresume, or end.");
}

PLUGIN_EXPORT void Finalize(void* data)
{
    auto measure = static_cast<Measure*>(data);
    const auto name = ClassName(measure);
    delete measure;
    UnregisterClassW(name.c_str(), Module());
}
