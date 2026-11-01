#pragma once
#include <Windows.h>
#include <string>
#include <vector>

// Owns only the files created by this extraction; never recursively deletes a path.
struct Animation
{
    std::wstring directory;
    std::vector<std::wstring> frames;
    // Per-frame display durations in milliseconds, with a 10 ms minimum.
    std::vector<UINT> delays;
    ~Animation();
    Animation() = default;
    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;
};

void ExtractAnimation(const std::wstring& path, Animation& output);
