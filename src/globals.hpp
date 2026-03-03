#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <cstdio>
#include <chrono>
#include <format>
#include <string>

inline HANDLE PHANDLE = nullptr;

#define LOG_PATH "/mnt/shared/hyprloading/hyprloading.log"

inline void logToFile(const std::string& msg) {
    FILE* f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    auto        now = std::chrono::system_clock::now();
    std::time_t t   = std::chrono::system_clock::to_time_t(now);
    std::tm     tm  = *std::localtime(&t);
    auto        ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count(), msg.c_str());
    fclose(f);
}
