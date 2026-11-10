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
#include <array>
#include <wincodec.h>
#include <wrl/client.h>

// Only Rainmeter's host boundary is mocked; the real decoder, timers, message
// window, commands, reload logic, and cleanup run unchanged.
enum { LOG_ERROR = 1, LOG_WARNING = 2 };
std::map<std::wstring, std::wstring> options;
std::vector<std::wstring> commands, errors;
size_t optionReads = 0;
std::map<std::wstring, std::wstring> sectionValues;
std::wstring resolvedOption;
std::wstring testResources;
void* RmGetSkin(void* rm) { return rm; }
double RmReadDouble(void* rm, LPCWSTR name, double value) { return RmReadFormula(rm, name, value); }
LPCWSTR __stdcall RmReadString(void*, LPCWSTR key, LPCWSTR fallback, BOOL replaceMeasures)
{
    ++optionReads;
    auto found = options.find(key);
    if (found == options.end()) return fallback;
    if (!replaceMeasures) return found->second.c_str();
    resolvedOption = found->second;
    for (const auto& entry : sectionValues)
    {
        const auto token = L"[" + entry.first + L"]";
        size_t pos = 0;
        while ((pos = resolvedOption.find(token, pos)) != std::wstring::npos)
        {
            resolvedOption.replace(pos, token.size(), entry.second);
            pos += entry.second.size();
        }
    }
    return resolvedOption.c_str();
}
double __stdcall RmReadFormula(void*, LPCWSTR key, double fallback)
{
    ++optionReads;
    auto found = options.find(key);
    return found == options.end() ? fallback : std::stod(found->second);
}
LPCWSTR __stdcall RmReplaceVariables(void*, LPCWSTR) { return testResources.c_str(); }
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
void AssertProtected(const std::wstring& path)
{
    Assert(!ReadPixels(path).empty(), "Protected file remains readable through WIC");
    Assert(!DeleteFileW(path.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION, "Active file denies deletion");
    Assert(!MoveFileW(path.c_str(), (path + L".renamed").c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
        "Active file denies renaming");
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD failure = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Assert(writer == INVALID_HANDLE_VALUE && failure == ERROR_SHARING_VIOLATION, "Active file denies overwriting");
}
void ExtractTestAnimation(const std::wstring& path, Animation& output, const std::function<bool()>& cancelled = {},
    const std::function<void(const Animation&)>& firstFrame = {})
{
    ExtractAnimation(path, testResources, output, cancelled, firstFrame);
}
struct TestSkin
{
    std::filesystem::path skin, resources, temporary;
    std::wstring oldTemp, oldTmp;
    bool hadTemp = false, hadTmp = false;
    TestSkin()
    {
        wchar_t executable[MAX_PATH]{};
        Assert(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "Find test build directory");
        GUID id{}; Assert(SUCCEEDED(CoCreateGuid(&id)), "Create test skin identifier");
        wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
        skin = std::filesystem::path(executable).parent_path() / (std::wstring(L"test-skin-") + guid);
        Assert(std::filesystem::create_directory(skin), "Create isolated test skin");
        resources = skin / L"@Resources";
        testResources = resources.wstring();
        temporary = skin / L"WindowsTemp";
        std::filesystem::create_directory(temporary);
        auto save = [](const wchar_t* name, std::wstring& value) {
            const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
            if (!length) return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
            value.resize(length);
            GetEnvironmentVariableW(name, value.data(), length);
            value.resize(length - 1);
            return true;
        };
        hadTemp = save(L"TEMP", oldTemp); hadTmp = save(L"TMP", oldTmp);
        SetEnvironmentVariableW(L"TEMP", temporary.c_str());
        SetEnvironmentVariableW(L"TMP", temporary.c_str());
    }
    ~TestSkin()
    {
        SetEnvironmentVariableW(L"TEMP", hadTemp ? oldTemp.c_str() : nullptr);
        SetEnvironmentVariableW(L"TMP", hadTmp ? oldTmp.c_str() : nullptr);
        RemoveDirectoryW(temporary.c_str());
        RemoveDirectoryW(resources.c_str());
        RemoveDirectoryW(skin.c_str());
    }
};
int wmain(int argc, wchar_t** argv)
{
    try
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        TestSkin testSkin;
        if (argc == 2)
        {
            Animation sample;
            ExtractTestAnimation(argv[1], sample);
            std::cout << "Sample decoded: " << sample.frames.size() << " frames\n";
        }
        auto fixture = [](const wchar_t* name) { return std::filesystem::absolute(std::filesystem::path(L"tests/fixtures") / name).wstring(); };
        {
            Animation active, orphan;
            ExtractTestAnimation(fixture(L"blend.webp"), active);
            ExtractTestAnimation(fixture(L"blend.webp"), orphan);
            const std::filesystem::path stale = orphan.directory;
            // Model process termination: Windows releases the owner handle,
            // but the Animation destructor never gets to remove its files.
            orphan.frameLocks.clear(); orphan.previewLock.Reset();
            CloseHandle(orphan.ownerFile);
            orphan.ownerFile = INVALID_HANDLE_VALUE;
            orphan.directory.clear(); orphan.temporaryRoot.clear();
            orphan.preview.clear(); orphan.frames.clear();
            const auto unrelated = stale / L"keep.txt";
            { std::ofstream file(unrelated); file << "Unrelated data"; }
            CleanupOrphanAnimations(testResources);
            Assert(std::filesystem::exists(unrelated) && std::filesystem::exists(stale / L"frame-0.bmp"),
                "Orphan recovery preserves entire folder when unexpected files exist");
            std::filesystem::remove(unrelated);
            {
                Animation next;
                ExtractTestAnimation(fixture(L"blend.webp"), next);
                Assert(!std::filesystem::exists(stale), "Next extraction recovers unlocked orphan folder");
                Assert(std::filesystem::exists(active.frames[0]), "Orphan recovery preserves active extraction");
            }
            const auto legacy = std::filesystem::path(active.temporaryRoot) /
                L"Rainmeter-Animate-{00000000-0000-0000-0000-000000000001}";
            std::filesystem::create_directory(legacy);
            { std::ofstream file(legacy / L"frame-0.bmp"); file << "Unverified legacy data"; }
            CleanupOrphanAnimations(testResources);
            Assert(std::filesystem::exists(legacy / L"frame-0.bmp"), "Unmarked legacy folders are preserved");
            { std::ofstream file(legacy / L".animate-owner"); file << "Invalid marker"; }
            CleanupOrphanAnimations(testResources);
            Assert(std::filesystem::exists(legacy / L"frame-0.bmp"), "Invalid ownership marker is preserved");
            std::filesystem::remove(legacy / L"frame-0.bmp");
            std::filesystem::remove(legacy / L".animate-owner");
            std::filesystem::remove(legacy);
        }
        {
            const auto temp = testSkin.temporary;
            std::array<std::unique_ptr<Animation>, 2> animations;
            std::array<std::exception_ptr, 2> failures;
            std::array<std::thread, 2> workers;
            for (size_t i = 0; i < workers.size(); ++i)
                workers[i] = std::thread([&, i] {
                    try
                    {
                        animations[i] = std::make_unique<Animation>();
                        ExtractTestAnimation(fixture(L"blend.webp"), *animations[i]);
                    }
                    catch (...) { failures[i] = std::current_exception(); }
                });
            for (auto& worker : workers) worker.join();
            for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
            Assert(!std::filesystem::exists(testSkin.resources) && std::filesystem::is_directory(temp),
                "Uses Windows Temp without creating skin resources");
            Assert(std::filesystem::path(animations[0]->directory).parent_path() == temp &&
                animations[0]->directory != animations[1]->directory, "Concurrent measures use isolated Windows Temp frame folders");
            animations[0].reset();
            Assert(std::filesystem::exists(animations[1]->frames[0]), "One measure's cleanup preserves another measure's frames");
            animations[1].reset();
            Assert(std::filesystem::is_directory(temp) && std::filesystem::is_empty(temp),
                "Last extraction removes its folder but preserves Windows Temp");
            std::filesystem::create_directory(temp);
            const auto unrelated = temp / L"unrelated.txt";
            { std::ofstream file(unrelated); file << "Keep this file"; }
            {
                Animation animation;
                ExtractTestAnimation(fixture(L"disposal.gif"), animation);
            }
            Assert(std::filesystem::exists(unrelated), "Cleanup preserves unrelated Temp files");
            std::filesystem::remove(unrelated);
            std::filesystem::remove(temp);
            const auto blocked = testSkin.skin / L"blocked-resources";
            { std::ofstream file(blocked); file << "Not a directory"; }
            {
                Animation animation;
                ExtractAnimation(fixture(L"blend.webp"), blocked.wstring(), animation);
                Assert(std::filesystem::path(animation.directory).parent_path() == temp,
                    "Extraction does not require writable skin resources");
            }
            std::filesystem::remove(blocked);
        }
        std::filesystem::create_directory(testSkin.resources);
        {
            const auto legacyRoot = testSkin.resources / L"Temp";
            const auto legacy = legacyRoot / L"Rainmeter-Animate-{00000000-0000-0000-0000-000000000002}";
            std::filesystem::create_directories(legacy);
            { std::ofstream file(legacy / L".animate-owner", std::ios::binary); file << "Rainmeter Animate extraction owner v1\r\n"; }
            { std::ofstream file(legacy / L"frame-0.bmp"); file << "Legacy generated data"; }
            CleanupOrphanAnimations(testResources);
            Assert(!std::filesystem::exists(legacyRoot) && std::filesystem::exists(testSkin.resources),
                "Windows Temp migration also cleans marked legacy skin folders and keeps resources");
        }
        {
            // Warm the Windows codecs before checking repeated extraction leases.
            { Animation warm; ExtractTestAnimation(fixture(L"blend.webp"), warm); }
            DWORD before = 0, after = 0;
            Assert(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "Read handle count before stress test");
            for (int i = 0; i < 30; ++i) { Animation animation; ExtractTestAnimation(fixture(L"blend.webp"), animation); }
            Assert(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after <= before + 2,
                "Repeated extraction and cleanup do not leak file handles");
            std::cout << "Frame-lock stress: handles before " << before << "; after " << after << "\n";
        }
        for (const auto* name : {L"blend.apng", L"poster.png", L"disposal.gif"})
        {
            std::wstring dir;
            {
                Animation animation;
                ExtractTestAnimation(fixture(name), animation);
                dir = animation.directory;
                for (const auto& frame : animation.frames) AssertProtected(frame);
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
            ExtractTestAnimation(fixture(L"denominator-zero.apng"), zero);
            ExtractTestAnimation(fixture(L"no-delay.gif"), missing);
            Assert(zero.delays[0] == 80, "APNG denominator zero means 100");
            Assert(missing.delays[0] == 100, "Missing GIF delay defaults to 100 ms");
        }
        {
            Animation alpha;
            ExtractTestAnimation(fixture(L"alpha.apng"), alpha);
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
        {
            Animation webp;
            bool preview = false;
            ExtractTestAnimation(fixture(L"blend.webp"), webp, {}, [&](const Animation& first) {
                Assert(first.frames.size() == 1 && first.delays[0] == 80 && !first.preview.empty(), "WebP first-frame preview");
                preview = true;
            });
            Assert(preview && webp.frames.size() == 4 && webp.delays == std::vector<UINT>({80,250,10,333}),
                "WebP millisecond timing, zero-delay minimum, frame count");
            Assert(ReadPixels(webp.frames[0]) == std::vector<BYTE>({0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255}),
                "WebP first full canvas");
            for (const auto& frame : webp.frames) AssertProtected(frame);
            AssertProtected(webp.preview);
            const auto mixed = ReadPixels(webp.frames[1]);
            Assert(mixed[0] == 128 && mixed[2] == 127 && mixed[3] == 255 && mixed[8] == 0 && mixed[10] == 255,
                "WebP OVER blends partial alpha and retains uncovered canvas");
            Assert(ReadPixels(webp.frames[2]) == std::vector<BYTE>({0,0,0,0,0,0,0,0,0,255,0,255,0,255,0,255}),
                "WebP BACKGROUND disposal and frame offset");
            Assert(ReadPixels(webp.frames[3]) == std::vector<BYTE>(16,0), "WebP transparent SOURCE replaces canvas");
            Animation alpha, lossy;
            ExtractTestAnimation(fixture(L"alpha.webp"), alpha);
            const auto rgba = ReadPixels(alpha.frames[0]);
            Assert(rgba[3] == 0 && rgba[7] == 64 && rgba[11] == 128 && rgba[15] == 255 && rgba[12] == 200,
                "WebP alpha preserved in BMP V5");
            ExtractTestAnimation(fixture(L"lossy.webp"), lossy);
            const auto rgb = ReadPixels(lossy.frames[0]);
            Assert(lossy.frames.size() == 2 && lossy.delays == std::vector<UINT>({40,60}) && rgb[2] > 240 && rgb[3] == 255,
                "Lossy WebP decoding");
            Animation sample;
            ExtractTestAnimation(L"examples/Animate/@Resources/DancingBanana.webp", sample);
            Assert(sample.frames.size() == 8, "Bundled animated WebP example decodes");
        }
        for (const auto* name : {L"bad.webp", L"truncated.webp", L"oversized.webp", L"static.webp", L"excessive-duration.webp"})
        {
            std::wstring dir;
            {
                Animation bad;
                bool rejected = false;
                try { ExtractTestAnimation(fixture(name), bad); } catch (const std::runtime_error&) { rejected = true; }
                dir = bad.directory;
                Assert(rejected, "Reject invalid, static, oversized, or excessive-duration WebP");
            }
            Assert(dir.empty() || !std::filesystem::exists(dir), "Rejected WebP removes partial output");
        }
        {
            std::wstring dir;
            bool cancelled = false;
            {
                Animation partial;
                try { ExtractTestAnimation(fixture(L"blend.webp"), partial, [&] { return !partial.frames.empty(); }); }
                catch (const AnimationCancelled&) { cancelled = true; }
                dir = partial.directory;
            }
            Assert(cancelled && !std::filesystem::exists(dir), "WebP cancellation cleans extracted frames");
        }
        std::wstring badDir;
        {
            Animation bad;
            bool rejected = false;
            try { ExtractTestAnimation(fixture(L"bad.apng"), bad); } catch (...) { rejected = true; }
            badDir = bad.directory;
            Assert(rejected, "Reject corrupted PNG");
        }
        Assert(!std::filesystem::exists(badDir), "Failed extraction cleans partial files");
        {
            Animation previewed;
            bool shown = false;
            std::vector<BYTE> original;
            ExtractTestAnimation(L"examples/Animate/@Resources/Loading.gif", previewed, {}, [&](const Animation& first) {
                Assert(first.frames.size() == 1 && !first.preview.empty(), "Preview is published with only the first frame extracted");
                AssertProtected(first.frames[0]);
                AssertProtected(first.preview);
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
                try { ExtractTestAnimation(fixture(L"blend.apng"), partial, [&] { return !partial.frames.empty(); }); }
                catch (const AnimationCancelled&) { cancelled = true; }
                cancelledDir = partial.directory;
            }
            Assert(cancelled && !std::filesystem::exists(cancelledDir), "Cancelled extraction removes partial frame output");
        }

        {
            options = {{L"ImageName",fixture(L"blend.webp")},{L"MeterName",L"PrefetchImage"},{L"Speed",L"75"},{L"Loop",L"-1"},{L"DisplayLoading",L"1"}};
            void* prefetch = nullptr;
            const auto reads = optionReads, displays = commands.size();
            Initialize(&prefetch, &options);
            auto* idle = static_cast<Measure*>(prefetch);
            Assert(optionReads > reads && idle->loading && !idle->started && idle->meter == L"PrefetchImage" &&
                idle->speed == 75 && idle->loop == -1 && idle->displayLoading, "Creation reads all options and starts asynchronous prefetch");
            Until([&] { return !idle->loading; }, "Creation-time prefetch finishes");
            Assert(idle->animation && idle->index == 0 && !idle->playing && commands.size() == displays + 1 &&
                commands.back().find(L"frame-0.bmp") != std::wstring::npos,
                "Completed prefetch displays only the real first frame, even with DisplayLoading enabled");
            Pump(120);
            Assert(idle->index == 0 && commands.size() == displays + 1 && Update(prefetch) == 75,
                "Prefetched frame remains still until Start");
            ExecuteBang(prefetch, L"start");
            Assert(idle->playing && idle->index == 0 && !idle->loading, "Start plays already-extracted frames without reloading");
            std::weak_ptr<AsyncLoader> prefetchCleanup = idle->loader;
            Finalize(prefetch);
            Until([&] { return prefetchCleanup.expired(); }, "Prefetch cleanup completes");
        }
        {
            sectionValues[L"Icon"] = L"blend";
            options = {{L"ImageName",fixture(L"[Icon].webp")},{L"MeterName",L"ResolvedImage"}};
            void* resolved = nullptr;
            Initialize(&resolved, &options);
            auto* idle = static_cast<Measure*>(resolved);
            Until([&] { return !idle->loading; }, "Existing section-variable filename prefetches");
            Assert(idle->animation && idle->displayed && !idle->playing && !idle->deferred,
                "Section-variable inputs that already resolve are loaded immediately");
            std::weak_ptr<AsyncLoader> resolvedCleanup = idle->loader;
            Finalize(resolved);
            Until([&] { return resolvedCleanup.expired(); }, "Resolved-variable cleanup completes");
        }
        {
            sectionValues[L"Icon"] = L"";
            options = {{L"ImageName",fixture(L"[Icon].webp")},{L"MeterName",L"DeferredImage"},{L"Loop",L"-1"}};
            void* deferred = nullptr; double deferredMaximum = -1;
            const auto reads = optionReads, displays = commands.size();
            Initialize(&deferred, &options);
            auto* idle = static_cast<Measure*>(deferred);
            const auto creationReads = optionReads;
            for (int i = 0; i < 20; ++i) { Reload(deferred, &options, &deferredMaximum); Update(deferred); Pump(1); }
            ExecuteBang(deferred, L"pause");
            ExecuteBang(deferred, L"end");
            Assert(idle->deferred && !idle->started && !idle->window && !idle->loader && !idle->worker.joinable() &&
                idle->source.empty() && idle->meter == L"DeferredImage" && creationReads > reads && optionReads == creationReads &&
                commands.size() == displays && std::wstring(GetString(deferred)).empty() && deferredMaximum == 0 && errors.empty(),
                "Unresolved section-variable input reads options once, then stays dormant without errors or meter commands");
            sectionValues[L"Icon"] = L"blend";
            ExecuteBang(deferred, L"start");
            Assert(idle->started && idle->loading && commands.size() == displays, "FinishAction Start resolves current section variables");
            Until([&] { return !idle->loading; }, "Deferred source loads");
            Assert(idle->animation && idle->playing && idle->source == fixture(L"blend.webp") && errors.empty(),
                "First Start resolves the current source without a preceding Reload");
            options[L"ImageName"] = fixture(L"blend.apng");
            options[L"Speed"] = L"75";
            ExecuteBang(deferred, L"start");
            Until([&] { return !idle->loading; }, "Second Start loads updated options");
            Assert(idle->animation && idle->playing && idle->source == fixture(L"blend.apng") && Update(deferred) == 75,
                "Every Start rereads current source and options without DynamicVariables");
            std::weak_ptr<AsyncLoader> deferredCleanup = idle->loader;
            Finalize(deferred);
            Until([&] { return deferredCleanup.expired(); }, "Deferred-start resources clean up");
            sectionValues.clear();
        }
        for (const auto* filename : {L".webp",L".apng",L".gif",L".WEBP",L".APNG",L".GIF"})
        {
            options = {{L"ImageName",(testSkin.resources / filename).wstring()},{L"MeterName",L"ExtensionImage"}};
            void* deferred = nullptr;
            const auto displays = commands.size();
            Initialize(&deferred, &options);
            auto* idle = static_cast<Measure*>(deferred);
            const auto reads = optionReads;
            for (int i = 0; i < 10; ++i) { double maximum = 0; Reload(deferred, &options, &maximum); Update(deferred); Pump(1); }
            Assert(idle->deferred && !idle->loader && !idle->window && idle->source.empty() && optionReads == reads &&
                commands.size() == displays && errors.empty(), "Extension-only filename waits silently without workers, retries, or meter commands");
            options[L"ImageName"] = fixture(L"blend.webp");
            ExecuteBang(deferred, L"start");
            Until([&] { return !idle->loading; }, "Extension-only placeholder resolves on Start");
            Assert(idle->animation && idle->playing && errors.empty(), "Start recovers extension-only placeholders");
            std::weak_ptr<AsyncLoader> deferredCleanup = idle->loader;
            Finalize(deferred);
            Until([&] { return deferredCleanup.expired(); }, "Extension-only cleanup completes");
        }
        {
            sectionValues[L"Icon"] = L"";
            options = {{L"ImageName",fixture(L"[Icon].webp")},{L"MeterName",L"EarlyStartImage"}};
            void* deferred = nullptr;
            Initialize(&deferred, &options);
            auto* idle = static_cast<Measure*>(deferred);
            Assert(idle->deferred && errors.empty(), "Unready section variable is silent at creation");
            ExecuteBang(deferred, L"start");
            Until([&] { return !idle->loading; }, "Premature Start fails once");
            Assert(errors.size() == 1 && !idle->animation && !idle->displayed, "Explicit Start with an unready source reports one error");
            const auto revision = idle->loadRevision;
            for (int i = 0; i < 20; ++i) { double maximum = 0; Reload(deferred, &options, &maximum); Update(deferred); Pump(1); }
            Assert(errors.size() == 1 && idle->loadRevision == revision, "Failed Start remains stopped without automatic retries");
            sectionValues[L"Icon"] = L"blend";
            ExecuteBang(deferred, L"start");
            Until([&] { return !idle->loading; }, "FinishAction Start retries the resolved source");
            Assert(idle->playing && errors.size() == 1, "Later valid Start recovers without extra errors");
            std::weak_ptr<AsyncLoader> deferredCleanup = idle->loader;
            Finalize(deferred);
            Until([&] { return deferredCleanup.expired(); }, "Premature-Start cleanup completes");
            errors.clear(); sectionValues.clear();
        }
        {
            sectionValues[L"Icon"] = L"bad";
            options = {{L"ImageName",fixture(L"[Icon].apng")},{L"MeterName",L"CorruptImage"}};
            void* corrupt = nullptr;
            Initialize(&corrupt, &options);
            auto* idle = static_cast<Measure*>(corrupt);
            Until([&] { return !idle->loading; }, "Existing corrupt variable input fails prefetch");
            Assert(errors.size() == 1 && !idle->animation && !idle->displayed && !idle->deferred,
                "A section variable does not suppress corrupt-file errors when its file exists");
            for (int i = 0; i < 20; ++i) { double maximum = 0; Reload(corrupt, &options, &maximum); Update(corrupt); Pump(1); }
            Assert(errors.size() == 1, "Corrupt prefetch failure logs once and does not retry on updates");
            std::weak_ptr<AsyncLoader> corruptCleanup = idle->loader;
            Finalize(corrupt);
            Until([&] { return corruptCleanup.expired(); }, "Corrupt prefetch cleanup completes");
            errors.clear(); sectionValues.clear();
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
        options[L"ImageName"] = fixture(L"blend.webp");
        options[L"Speed"] = L"0";
        options[L"Loop"] = L"0"; // Overrides the WebP's embedded three loops.
        Reload(data, &options, &maximum);
        ExecuteBang(data, L"start");
        Until([&] { return !m->loading; }, "WebP asynchronous source switch finishes");
        Reload(data, &options, &maximum);
        Assert(m->animation && m->playing && maximum == 333 && Update(data) == 80 &&
            std::wstring(GetString(data)) == fixture(L"blend.webp"), "WebP source path and effective timing outputs");
        ExecuteBang(data, L"pause");
        const auto webpFrame = m->index.load();
        Pump(100);
        Assert(!m->playing && m->index == webpFrame, "WebP pause suspends playback");
        ExecuteBang(data, L"PauseResume");
        Until([&] { return m->completed.load(); }, "WebP plays once despite embedded loop count");
        Assert(m->index == 3 && m->repeats == 0 && Update(data) == 333, "WebP loop override and final delay");
        ExecuteBang(data, L"end");
        Assert(m->index == 0 && !m->playing && Update(data) == 80, "WebP End resets to first frame");
        auto finalDir = m->animation->directory;
        std::weak_ptr<AsyncLoader> cleanup = m->loader;
        Finalize(data);
        Until([&] { return cleanup.expired(); }, "Background cleanup completes");
        Assert(!std::filesystem::exists(finalDir), "Finalize removes temporary folder");
        Assert(std::filesystem::is_empty(testSkin.temporary) && std::filesystem::is_directory(testSkin.resources),
            "Finalize removes generated folders while preserving Windows Temp and skin resources");
        Pump(60); // Pending timer messages after finalization must be harmless.
        Assert(errors.empty(), "No unexpected plugin errors");
        // Weather skins can issue Start on refresh before WebParser supplies an
        // icon code, temporarily leaving an empty or extension-only filename.
        for (const auto& invalid : {std::wstring(), (testSkin.resources / L"missing.webp").wstring(), fixture(L"bad.apng")})
        {
            options = {{L"ImageName", invalid},{L"MeterName",L"WeatherImage"},{L"Loop",L"-1"}};
            Initialize(&data, &options);
            m = static_cast<Measure*>(data);
            Reload(data, &options, &maximum);
            const size_t expectedErrors = invalid.empty() ? 2 : 1;
            ExecuteBang(data, L"start");
            Until([&] { return !m->loading; }, "Invalid weather source completes its load attempt");
            Assert(errors.size() == expectedErrors && !m->playing && m->startWhenReady, "Refresh Start logs one error and waits for a valid icon");
            Assert(!m->displayed, "Failed first load does not display or update an empty meter");
            const auto revision = m->loadRevision;
            const auto failedCommands = commands.size();
            for (int i = 0; i < 50; ++i) { Reload(data, &options, &maximum); Update(data); Pump(1); }
            Assert(errors.size() == expectedErrors && m->loadRevision == revision && commands.size() == failedCommands,
                "Skin updates neither retry an invalid source nor repeat errors or image bangs");
            ExecuteBang(data, L"StArT");
            ExecuteBang(data, L"start");
            Until([&] { return !m->loading; }, "Explicit Start retries finish");
            Assert(errors.size() == expectedErrors + 2, "Exactly one source error for each explicit Start, including queued Starts");
            ExecuteBang(data, L"end");
            options[L"ImageName"] = fixture(L"blend.webp");
            Reload(data, &options, &maximum);
            Until([&] { return !m->loading; }, "Valid replacement after End loads");
            Assert(m->animation && !m->playing && errors.size() == expectedErrors + 2, "End cancels waiting playback");
            ExecuteBang(data, L"start");
            options[L"ImageName"] = invalid.empty() ? L"" : (testSkin.resources / L".webp").wstring();
            Reload(data, &options, &maximum);
            Until([&] { return !m->loading; }, "Failed dynamic replacement loads silently");
            const size_t dynamicErrors = expectedErrors + 2 + (invalid.empty() ? 0 : 1);
            Assert(errors.size() == dynamicErrors && m->startWhenReady && commands.back().find(L"ImageName \"\"") != std::wstring::npos,
                "Dynamic invalid source stays quiet and clears deleted frame paths");
            options[L"ImageName"] = fixture(L"blend.webp");
            Reload(data, &options, &maximum);
            Until([&] { return !m->loading; }, "WeatherParser valid icon loads");
            Assert(m->animation && m->playing && errors.size() == dynamicErrors, "Original playback request resumes when WebParser supplies a valid source");
            cleanup = m->loader;
            Finalize(data);
            Until([&] { return cleanup.expired(); }, "Failed-source test cleanup completes");
            errors.clear();
        }
        {
            const auto repair = (testSkin.resources / L"repaired.webp").wstring();
            options = {{L"ImageName",repair},{L"MeterName",L"WeatherImage"},{L"Loop",L"-1"}};
            Initialize(&data, &options);
            m = static_cast<Measure*>(data);
            Reload(data, &options, &maximum);
            Until([&] { return !m->loading; }, "Unstarted failed source finishes silently");
            Assert(m->loader && !m->animation && errors.size() == 1, "Missing literal source logs once during creation");
            std::filesystem::copy_file(fixture(L"blend.webp"), repair);
            ExecuteBang(data, L"start");
            Until([&] { return !m->loading; }, "Repaired source reloads on Start");
            Assert(m->animation && m->playing && errors.size() == 1, "Start retries a repaired filename without a stale error");
            cleanup = m->loader;
            Finalize(data);
            Until([&] { return cleanup.expired(); }, "Repaired-source cleanup completes");
            std::filesystem::remove(repair);
            errors.clear();
        }
        {
            options = {{L"ImageName",fixture(L"blend.webp")},{L"MeterName",L"WeatherImage"},{L"Loop",L"-1"},{L"Speed",L"30000"}};
            Initialize(&data, &options);
            m = static_cast<Measure*>(data);
            Reload(data, &options, &maximum);
            ExecuteBang(data, L"start");
            Until([&] { return !m->loading; }, "Protected-frame test starts playback");
            for (const auto& frame : m->animation->frames) AssertProtected(frame);
            const auto folder = m->animation->directory;
            Assert(!RemoveDirectoryW(folder.c_str()), "Active frame folder cannot be removed");
            Assert(Update(data) == 30000 && m->playing && errors.empty(), "Deletion attempts leave playback healthy");
            cleanup = m->loader;
            Finalize(data);
            Until([&] { return cleanup.expired(); }, "Protected-frame cleanup completes");
            Assert(!std::filesystem::exists(folder), "Unload closes locks and deletes frames");
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--large")
        {
            options = {{L"ImageName",argv[2]},{L"MeterName",L"LargeImage"},{L"Loop",L"-1"},{L"DisplayLoading",L"1"}};
            Initialize(&data, &options);
            m = static_cast<Measure*>(data);
            const auto began = GetTickCount64();
            const auto beganPrecise = std::chrono::steady_clock::now();
            Reload(data, &options, &maximum);
            ExecuteBang(data, L"start");
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
        std::cout << "PASS: GIF/APNG/WebP pixels, transparency, disposal, poster exclusion, corruption, timers, pause/resume, loops, reload, cleanup\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
