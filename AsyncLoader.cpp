#include "AsyncLoader.h"
#include <stdexcept>

namespace
{
bool SafeArgument(const std::wstring& text)
{
    return text.find_first_of(L"\"\r\n[]") == std::wstring::npos;
}
}

struct AsyncLoader::Context
{
    std::shared_ptr<AsyncLoader> loader;
    HMODULE module = nullptr;
};

std::shared_ptr<AsyncLoader> AsyncLoader::Create(HWND window, UINT message, std::wstring resources)
{
    auto loader = std::make_shared<AsyncLoader>();
    loader->window = window;
    loader->message = message;
    loader->resources = std::move(resources);
    auto context = std::make_unique<Context>();
    context->loader = loader;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Callback), &context->module))
        throw std::runtime_error("Cannot retain module for background extraction");
    // The tests compile this code into their EXE rather than loading a DLL.
    if (context->module == GetModuleHandleW(nullptr)) context->module = nullptr;
    PTP_WORK work = CreateThreadpoolWork(Callback, context.get(), nullptr);
    if (!work)
    {
        if (context->module) FreeLibrary(context->module);
        throw std::runtime_error("Cannot create background extraction worker");
    }
    context.release(); // Callback owns this context until all cleanup completes.
    SubmitThreadpoolWork(work);
    return loader;
}

void CALLBACK AsyncLoader::Callback(PTP_CALLBACK_INSTANCE instance, void* raw, PTP_WORK work)
{
    std::unique_ptr<Context> context(static_cast<Context*>(raw));
    CallbackMayRunLong(instance);
    // The DLL remains loaded through every destructor, even after Finalize.
    if (context->module) FreeLibraryWhenCallbackReturns(instance, context->module);
    const int priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    context->loader->Run();
    SetThreadPriority(GetCurrentThread(), priority);
    CloseThreadpoolWork(work); // Released after this callback returns.
}

UINT_PTR AsyncLoader::Request(std::wstring path, std::unique_ptr<Animation> retired)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto id = ++revision; // Cancels extraction of an older source.
    if (retired) garbage.push_back(std::move(retired));
    if (result) garbage.push_back(std::move(result));
    source = std::move(path);
    error.clear();
    ready = 0;
    preview.clear();
    originalPreview.clear();
    requested = !source.empty();
    condition.notify_one();
    return id;
}

bool AsyncLoader::Take(UINT_PTR id, std::unique_ptr<Animation>& loaded, std::wstring& failure)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (id != revision || ready != id) return false;
    loaded = std::move(result);
    failure = std::move(error);
    ready = 0;
    return true;
}

bool AsyncLoader::Preview(UINT_PTR id, std::wstring& path, std::wstring& original, UINT& delay)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (id != revision || preview.empty()) return false;
    path = preview;
    original = originalPreview;
    delay = previewDelay;
    return true;
}

void AsyncLoader::DiscardPreview(std::wstring path)
{
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(mutex);
    garbageFiles.push_back(std::move(path));
    condition.notify_one();
}

void AsyncLoader::Stop(std::unique_ptr<Animation> retired)
{
    std::lock_guard<std::mutex> lock(mutex);
    closing = true;
    ++revision;
    window = nullptr;
    requested = false;
    if (retired) garbage.push_back(std::move(retired));
    if (result) garbage.push_back(std::move(result));
    condition.notify_one();
}

void AsyncLoader::Retire(std::vector<std::unique_ptr<Animation>> animations)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& animation : animations) garbage.push_back(std::move(animation));
    condition.notify_one();
}

void AsyncLoader::Run()
{
    for (;;)
    {
        std::vector<std::unique_ptr<Animation>> retired;
        std::vector<std::wstring> retiredFiles;
        std::wstring path;
        UINT_PTR id;
        bool stop;
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return closing || requested || !garbage.empty() || !garbageFiles.empty(); });
            retired.swap(garbage);
            retiredFiles.swap(garbageFiles);
            stop = closing;
            id = revision;
            if (requested && !stop) path = source;
            requested = false;
        }
        for (const auto& file : retiredFiles) DeleteFileW(file.c_str());
        retired.clear(); // Potentially thousands of DeleteFile calls, always off the UI thread.
        if (stop) return;
        if (path.empty()) continue;
        auto cancelled = [&] { return closing || revision != id; };
        std::unique_ptr<Animation> loaded;
        std::wstring failure;
        try
        {
            loaded = std::make_unique<Animation>();
            ExtractAnimation(path, resources, *loaded, cancelled, [&](const Animation& animation) {
                if (!SafeArgument(animation.preview)) throw std::runtime_error("Temporary path contains unsupported bang delimiters");
                std::lock_guard<std::mutex> lock(mutex);
                if (cancelled()) return;
                preview = animation.preview;
                originalPreview = animation.frames.front();
                previewDelay = animation.delays.front();
                PostMessageW(window, message, id, 1);
            });
            for (const auto& frame : loaded->frames)
                if (!SafeArgument(frame)) throw std::runtime_error("Temporary path contains unsupported bang delimiters");
        }
        catch (const AnimationCancelled&) { loaded.reset(); }
        catch (const std::exception& exception)
        {
            loaded.reset();
            const std::string text = exception.what();
            failure = L"Animate: " + std::wstring(text.begin(), text.end()) + L". Source: " + path;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!cancelled())
            {
                result = std::move(loaded);
                error = std::move(failure);
                ready = id;
                PostMessageW(window, message, id, 0);
            }
        }
        // A superseded result is destroyed here on this worker, never on the UI.
    }
}
