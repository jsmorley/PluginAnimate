#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <exception>
#include <utility>

// Readable by Rainmeter, but not writable, renameable, or deletable while owned.
struct AnimationFileLock
{
    HANDLE value = INVALID_HANDLE_VALUE;
    AnimationFileLock() = default;
    explicit AnimationFileLock(HANDLE handle) : value(handle) {}
    AnimationFileLock(const AnimationFileLock&) = delete;
    AnimationFileLock& operator=(const AnimationFileLock&) = delete;
    AnimationFileLock(AnimationFileLock&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    AnimationFileLock& operator=(AnimationFileLock&& other) noexcept
    {
        if (this != &other) { Reset(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    void Reset() noexcept { if (value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE)); }
    ~AnimationFileLock() { Reset(); }
};

struct AnimationCancelled : std::exception
{
    const char* what() const noexcept override { return "Animation extraction cancelled"; }
};

// Owns only the files created by this extraction; never recursively deletes a path.
struct Animation
{
    std::wstring directory;
    std::wstring temporaryRoot;
    std::wstring preview;
    AnimationFileLock previewLock;
    std::vector<AnimationFileLock> frameLocks;
    HANDLE ownerFile = INVALID_HANDLE_VALUE;
    std::vector<std::wstring> frames;
    // Per-frame display durations in milliseconds, with a 10 ms minimum.
    std::vector<UINT> delays;
    ~Animation();
    Animation() = default;
    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;
};

// Best-effort recovery of marked, unlocked Windows Temp and legacy skin folders.
void CleanupOrphanAnimations(const std::wstring& resources);

void ExtractAnimation(const std::wstring& path, const std::wstring& resources, Animation& output, const std::function<bool()>& cancelled = {},
    const std::function<void(const Animation&)>& firstFrame = {});
