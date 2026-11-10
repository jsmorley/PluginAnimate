#pragma once
#include "Animation.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

// Owns extraction and deletion work independently of a Rainmeter measure's
// lifetime. No Rainmeter APIs are ever called from this worker.
class AsyncLoader
{
public:
    static std::shared_ptr<AsyncLoader> Create(HWND window, UINT message, std::wstring resources);
    UINT_PTR Request(std::wstring source, std::unique_ptr<Animation> retired, bool firstFrameOnly = false);
    bool Take(UINT_PTR revision, std::unique_ptr<Animation>& result, std::wstring& error);
    bool Preview(UINT_PTR revision, std::wstring& path, std::wstring& original, UINT& delay);
    void DiscardPreview(std::wstring path, AnimationFileLock protection);
    void Retire(std::vector<std::unique_ptr<Animation>> animations);
    void Stop(std::unique_ptr<Animation> retired);

private:
    struct Context;
    static void CALLBACK Callback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK work);
    void Run();
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<UINT_PTR> revision{0};
    std::atomic<bool> closing{false};
    HWND window = nullptr;
    UINT message = 0;
    bool requested = false;
    bool firstFrameOnly = false;
    UINT_PTR ready = 0;
    std::wstring source, error;
    std::wstring resources;
    std::wstring preview;
    std::wstring originalPreview;
    UINT previewDelay = 0;
    std::unique_ptr<Animation> result;
    std::vector<std::unique_ptr<Animation>> garbage;
    struct RetiredFile { std::wstring path; AnimationFileLock protection; };
    std::vector<RetiredFile> garbageFiles;
};
