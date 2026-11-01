#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <exception>

struct AnimationCancelled : std::exception
{
    const char* what() const noexcept override { return "Animation extraction cancelled"; }
};

// Owns only the files created by this extraction; never recursively deletes a path.
struct Animation
{
    std::wstring directory;
    std::wstring preview;
    std::vector<std::wstring> frames;
    // Per-frame display durations in milliseconds, with a 10 ms minimum.
    std::vector<UINT> delays;
    ~Animation();
    Animation() = default;
    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;
};

void ExtractAnimation(const std::wstring& path, Animation& output, const std::function<bool()>& cancelled = {},
    const std::function<void(const Animation&)>& firstFrame = {});
