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
constexpr UINT RetireMessage = WM_APP + 3;
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
    bool prefetching = false;
    bool startWhenReady = false;
    bool displayLoading = false;
    bool started = false;
    bool configured = false;
    bool deferred = false;
    bool displayed = false;
    uint64_t pendingStarts = 0;
    bool frameErrorAllowed = false;
    UINT_PTR loadRevision = 0;
    std::shared_ptr<AsyncLoader> loader;
    std::wstring source, meter, preview, originalPreview;
    UINT previewDelay = 0;
    std::unique_ptr<Animation> animation;
    std::vector<std::unique_ptr<Animation>> retired;

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
            if (loader)
            {
                loader->Retire(std::move(retired));
                loader->Stop(std::move(animation));
            }
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
    bool ValidateFrame(std::wstring& error)
    {
        if (!animation) return true;
        const DWORD attributes = GetFileAttributesW(animation->frames[index].c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
        StopPlayback();
        retired.push_back(std::move(animation));
        preview.clear(); originalPreview.clear();
        loading = false; paused = false; startWhenReady = true;
        pendingStarts = 0;
        if (loader) loadRevision = loader->Request(L"", {});
        if (frameErrorAllowed)
            error = L"Animate: Extracted frames are missing. Use Start to rebuild them. Source: " + source;
        frameErrorAllowed = false;
        return false;
    }
    // Rainmeter exposes Disabled/Paused as options, not through RmGet. Read
    // only these host flags before touching animation options or UI output.
    bool IsSuspended()
    {
        if (RmReadDouble(rm, L"Disabled", 0) == 0 && RmReadDouble(rm, L"Paused", 0) == 0) return false;
        std::lock_guard<std::mutex> lock(mutex);
        StopPlayback();
        startWhenReady = false; pendingStarts = 0; paused = false;
        if (loading && loader) loadRevision = loader->Request(L"", {});
        loading = false;
        return true;
    }
    void Show()
    {
        if (IsSuspended()) return;
        std::wstring command;
        std::wstring error;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ValidateFrame(error);
            const std::wstring loadingImage = started ? (displayLoading ? preview : originalPreview) : L"";
            const std::wstring image = loading && !loadingImage.empty() ? loadingImage :
                (animation ? animation->frames[index] : loadingImage);
            // An empty image actively clears a previous frame. Leaving a deleted
            // frame path on the meter makes Rainmeter log on every skin update.
            if (!meter.empty() && (!image.empty() || displayed))
            {
                command = L"[!SetOption \"" + meter + L"\" ImageName \"" + image + L"\"][!UpdateMeter \"" + meter + L"\"]";
                displayed = !image.empty();
                command += L"[!Redraw]";
            }
            // RmExecute is synchronous. This posted message runs after the meter
            // has stopped using the old paths, keeping deletion off the UI thread.
            if (!retired.empty() || (animation && !animation->preview.empty()))
                PostMessageW(window, RetireMessage, 0, 0);
        }
        if (!error.empty()) RmLog(rm, LOG_ERROR, error.c_str());
        // Execute last: meter actions can refresh/unload this measure during RmExecute.
        if (!command.empty()) RmExecute(skin, command.c_str());
    }
    void Loaded(UINT_PTR revision, LPARAM stage)
    {
        if (IsSuspended()) return;
        std::wstring error;
        uint64_t errorReports = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!loader || revision != loadRevision) return;
            if (stage == 1)
            {
                if (!loader->Preview(revision, preview, originalPreview, previewDelay)) return;
                // Creation-time prefetch displays only the completed first frame.
                if (!started) return;
            }
            else if (stage == 0)
            {
                std::unique_ptr<Animation> loaded;
                if (!loader->Take(revision, loaded, error)) return;
                if (animation) retired.push_back(std::move(animation));
                animation = std::move(loaded);
                prefetching = false;
                if (!error.empty()) { errorReports = (std::max)(uint64_t(1), pendingStarts); frameErrorAllowed = false; }
                pendingStarts = 0;
                loading = false;
                preview.clear();
                originalPreview.clear();
                if (animation && startWhenReady && !meter.empty()) StartPlayback();
                // Keep Start pending after a bad weather icon path. A later
                // WebParser source change can then begin playback automatically.
                if (animation) startWhenReady = false;
            }
        }
        for (uint64_t i = 0; i < errorReports; ++i) RmLog(rm, LOG_ERROR, error.c_str());
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
    if (message == RetireMessage && measure)
    {
        std::lock_guard<std::mutex> lock(measure->mutex);
        if (measure->loader)
        {
            if (measure->animation && !measure->animation->preview.empty())
                measure->loader->DiscardPreview(std::move(measure->animation->preview), std::move(measure->animation->previewLock));
            measure->loader->Retire(std::move(measure->retired));
        }
        return 0;
    }
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
bool HasSectionVariable(const std::wstring& text)
{
    for (size_t begin = text.find(L'['); begin != std::wstring::npos; begin = text.find(L'[', begin + 1))
    {
        const auto end = text.find(L']', begin + 1);
        if (end != std::wstring::npos && end > begin + 1) return true;
    }
    return false;
}
bool ExtensionOnly(const std::wstring& source)
{
    const auto separator = source.find_last_of(L"\\/");
    const wchar_t* filename = source.c_str() + (separator == std::wstring::npos ? 0 : separator + 1);
    return _wcsicmp(filename, L".webp") == 0 || _wcsicmp(filename, L".apng") == 0 || _wcsicmp(filename, L".gif") == 0;
}
bool StartRuntime(Measure& measure)
{
    measure.skin = RmGetSkin(measure.rm);
    const auto name = ClassName(&measure);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = Module();
    wc.lpszClassName = name.c_str();
    if (!measure.window && (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS))
        measure.window = CreateWindowExW(0, name.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &measure);
    if (!measure.window)
    {
        RmLog(measure.rm, LOG_ERROR, L"Animate: Cannot create display message window.");
        return false;
    }
    try
    {
        if (!measure.loader)
            measure.loader = AsyncLoader::Create(measure.window, LoadMessage, RmReplaceVariables(measure.rm, L"#@#"));
        if (!measure.worker.joinable()) measure.worker = std::thread(&Measure::Run, &measure);
    }
    catch (const std::exception&)
    {
        RmLog(measure.rm, LOG_ERROR, L"Animate: Cannot create background workers.");
        return false;
    }
    return true;
}
}

PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue);

PLUGIN_EXPORT void Initialize(void** data, void* rm)
{
    auto measure = std::make_unique<Measure>();
    measure->rm = rm;
    *data = measure.release();
    double maximum = 0;
    Reload(*data, rm, &maximum);
}

PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue)
{
    auto& m = *static_cast<Measure*>(data);
    if (m.IsSuspended()) { *maxValue = 0.0; return; }
    if (m.deferred && !m.started) { *maxValue = 0.0; return; }
    std::unique_lock<std::mutex> lock(m.mutex);
    bool show = false;
    std::wstring error;
    // Preserve section-variable syntax before Rainmeter substitutes its value.
    const std::wstring expression = RmReadString(rm, L"ImageName", L"", FALSE);
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
    const bool sourceChanged = !m.configured || source != m.source;
    const bool speedChanged = speed != m.speed;
    show = meter != m.meter || displayLoading != m.displayLoading;
    m.displayLoading = displayLoading;
    m.meter = std::move(meter); m.speed = speed; m.loop = loop;
    m.configured = true;
    const bool extensionOnly = ExtensionOnly(source);
    if (!m.started && (HasSectionVariable(expression) || extensionOnly))
    {
        const DWORD attributes = (source.empty() || extensionOnly) ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            m.deferred = true;
            m.StopPlayback();
            m.source.clear(); m.preview.clear(); m.originalPreview.clear();
            m.startWhenReady = false; m.pendingStarts = 0;
            if (m.animation) m.retired.push_back(std::move(m.animation));
            if (m.loader) m.loadRevision = m.loader->Request(L"", {});
            m.loading = false;
            *maxValue = 0.0;
            lock.unlock();
            m.Show();
            return;
        }
    }
    m.deferred = false;
    if (sourceChanged)
    {
        show = true;
        m.pendingStarts = 0;
        m.StopPlayback();
        m.startWhenReady = wasPlaying || m.startWhenReady;
        m.source = std::move(source);
        m.index = 0; m.repeats = 0; m.completed = false;
        m.preview.clear();
        m.originalPreview.clear();
        if (m.animation) m.retired.push_back(std::move(m.animation));
        if (!m.source.empty() && !m.loader) StartRuntime(m);
        m.prefetching = !m.started;
        if (m.loader) m.loadRevision = m.loader->Request(m.source, {}, m.prefetching);
        m.loading = m.loader && !m.source.empty();
        if (m.source.empty() && !m.started) error = L"Animate: ImageName is empty.";
    }
    if (m.meter.empty() || !m.animation) m.StopPlayback();
    else if (wasPlaying && speedChanged) m.StartPlayback();
    *maxValue = m.animation ? double(m.speed ? m.speed : *std::max_element(m.animation->delays.begin(), m.animation->delays.end())) : 0.0;
    lock.unlock();
    if (!error.empty()) RmLog(m.rm, LOG_ERROR, error.c_str());
    if (show) m.Show();
}

PLUGIN_EXPORT double Update(void* data)
{
    auto& m = *static_cast<Measure*>(data);
    if (!m.started && !m.animation) return 0.0;
    std::unique_lock<std::mutex> lock(m.mutex);
    std::wstring error;
    if (!m.ValidateFrame(error))
    {
        lock.unlock();
        if (!error.empty()) RmLog(m.rm, LOG_ERROR, error.c_str());
        m.Show();
        return 0.0;
    }
    if (m.animation) return double(m.speed ? m.speed : m.animation->delays[m.index]);
    return m.preview.empty() ? 0.0 : double(m.speed ? m.speed : m.previewDelay);
}

PLUGIN_EXPORT LPCWSTR GetString(void* data)
{
    // An unresolved section-variable source stays empty until Start.
    return static_cast<Measure*>(data)->source.c_str();
}

PLUGIN_EXPORT void ExecuteBang(void* data, LPCWSTR args)
{
    auto& m = *static_cast<Measure*>(data);
    if (m.IsSuspended()) return;
    if (_wcsicmp(args, L"start") == 0)
    {
        if (!m.started)
        {
            if (!StartRuntime(m)) return;
            m.started = true;
        }
        // Read and resolve the current options for every Start, even when
        // DynamicVariables is absent and the host has not called Reload again.
        double maximum = 0;
        Reload(data, m.rm, &maximum);
    }
    else if (!m.started) return;
    std::unique_lock<std::mutex> lock(m.mutex);
    if (_wcsicmp(args, L"start") == 0)
    {
        if (m.source.empty())
        {
            m.StopPlayback();
            m.index = 0; m.repeats = 0; m.completed = false; m.paused = false;
            m.startWhenReady = true;
            m.frameErrorAllowed = false;
            lock.unlock();
            RmLog(m.rm, LOG_ERROR, L"Animate: ImageName is empty.");
            m.Show();
            return;
        }
        if (!m.worker.joinable() || m.meter.empty()) return;
        m.frameErrorAllowed = true;
        m.index = 0; m.repeats = 0; m.completed = false; m.paused = false;
        if (m.prefetching || (m.animation && m.animation->firstFrameOnly && !m.loading))
        {
            m.StopPlayback();
            m.prefetching = false;
            m.pendingStarts = 1;
            m.startWhenReady = true;
            m.loading = true;
            m.loadRevision = m.loader->Request(m.source, {});
            return;
        }
        if (m.loading) { ++m.pendingStarts; m.StopPlayback(); m.startWhenReady = true; return; }
        if (!m.animation)
        {
            // Retry only on an explicit Start, so a repaired file can recover
            // without turning skin updates into repeated loads or log entries.
            if (m.loader)
            {
                m.pendingStarts = 1;
                m.startWhenReady = true;
                m.loading = true;
                m.loadRevision = m.loader->Request(m.source, {});
            }
            return;
        }
        m.StartPlayback();
        lock.unlock();
        m.Show();
    }
    else if (_wcsicmp(args, L"pause") == 0 || _wcsicmp(args, L"pauseresume") == 0)
    {
        const bool toggle = _wcsicmp(args, L"pauseresume") == 0;
        if (m.loading || !m.animation)
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
        m.pendingStarts = 0;
        m.frameErrorAllowed = false;
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
