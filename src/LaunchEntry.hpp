#pragma once

#include <string>
#include <chrono>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/render/Texture.hpp>

struct LaunchEntry {
    std::string                           appId;
    SP<CTexture>                          texture;
    std::chrono::steady_clock::time_point startTime;
    float                                 opacity     = 0.f;
    float                                 bounceOffset = 0.f;
    bool                                  fadingOut   = false;
    CBox                                  lastBox;
};
