#include "IconResolver.hpp"
#include "globals.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <format>

namespace fs = std::filesystem;

static const std::vector<std::string> DESKTOP_DIRS = {
    []() -> std::string {
        const char* home = std::getenv("HOME");
        return home ? std::string(home) + "/.local/share/applications" : "";
    }(),
    "/usr/share/applications",
    "/usr/local/share/applications",
    []() -> std::string {
        const char* home = std::getenv("HOME");
        return home ? std::string(home) + "/.local/share/flatpak/exports/share/applications" : "";
    }(),
    "/var/lib/flatpak/exports/share/applications",
};

static const std::vector<std::string> ICON_DIRS = {
    []() -> std::string {
        const char* home = std::getenv("HOME");
        return home ? std::string(home) + "/.local/share/icons" : "";
    }(),
    "/usr/share/icons",
    "/usr/local/share/icons",
};

static const std::vector<std::string> ICON_SIZES = {
    "48x48", "64x64", "32x32", "96x96", "128x128", "256x256", "scalable",
};

static const std::vector<std::string> ICON_EXTENSIONS = {
    ".png", ".svg",
};

std::optional<std::string> CIconResolver::resolveIconPath(const std::string& appId, int size) {
    logToFile(std::format("resolveIconPath: looking up '{}'", appId));

    // Check cache first
    auto it = m_cache.find(appId);
    if (it != m_cache.end()) {
        logToFile(std::format("resolveIconPath: cache hit for '{}' → {}", appId, it->second));
        return it->second;
    }

    // Step 1: Find the .desktop file and extract Icon= value
    auto desktopPath = resolveDesktopFile(appId);
    if (!desktopPath) {
        logToFile(std::format("resolveIconPath: no .desktop file for '{}'", appId));
        return std::nullopt;
    }

    logToFile(std::format("resolveIconPath: found desktop file: {}", *desktopPath));

    auto iconName = parseIconFromDesktop(*desktopPath);
    if (!iconName) {
        logToFile(std::format("resolveIconPath: no Icon= in {}", *desktopPath));
        return std::nullopt;
    }

    logToFile(std::format("resolveIconPath: Icon={}", *iconName));

    // Step 2: If icon is an absolute path, use directly
    if (iconName->starts_with("/")) {
        if (fs::exists(*iconName)) {
            m_cache[appId] = *iconName;
            return *iconName;
        }
        return std::nullopt;
    }

    // Step 3: Resolve via icon theme
    auto iconPath = resolveIconFromTheme(*iconName, size);
    if (iconPath) {
        m_cache[appId] = *iconPath;
        return *iconPath;
    }

    // Step 4: Fallback to /usr/share/pixmaps
    for (const auto& ext : ICON_EXTENSIONS) {
        auto pixmapPath = "/usr/share/pixmaps/" + *iconName + ext;
        if (fs::exists(pixmapPath)) {
            m_cache[appId] = pixmapPath;
            return pixmapPath;
        }
    }

    return std::nullopt;
}

std::optional<std::string> CIconResolver::resolveDesktopFile(const std::string& appId) {
    std::string appIdLower = appId;
    std::transform(appIdLower.begin(), appIdLower.end(), appIdLower.begin(), ::tolower);

    for (const auto& dir : DESKTOP_DIRS) {
        if (dir.empty() || !fs::exists(dir))
            continue;

        try {
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".desktop")
                    continue;

                std::string stem = entry.path().stem().string();
                std::string stemLower = stem;
                std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);

                // Exact match on filename stem
                if (stemLower == appIdLower)
                    return entry.path().string();

                // Match with dots replaced (e.g., org.mozilla.firefox -> firefox)
                auto lastDot = stemLower.rfind('.');
                if (lastDot != std::string::npos && stemLower.substr(lastDot + 1) == appIdLower)
                    return entry.path().string();
            }
        } catch (...) {
            continue;
        }
    }

    // Second pass: check StartupWMClass inside .desktop files
    for (const auto& dir : DESKTOP_DIRS) {
        if (dir.empty() || !fs::exists(dir))
            continue;

        try {
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".desktop")
                    continue;

                std::ifstream file(entry.path());
                std::string   line;
                while (std::getline(file, line)) {
                    if (line.starts_with("StartupWMClass=")) {
                        std::string wmclass = line.substr(15);
                        std::string wmclassLower = wmclass;
                        std::transform(wmclassLower.begin(), wmclassLower.end(), wmclassLower.begin(), ::tolower);
                        if (wmclassLower == appIdLower)
                            return entry.path().string();
                    }
                }
            }
        } catch (...) {
            continue;
        }
    }

    return std::nullopt;
}

std::optional<std::string> CIconResolver::parseIconFromDesktop(const std::string& desktopPath) {
    std::ifstream file(desktopPath);
    if (!file.is_open())
        return std::nullopt;

    std::string line;
    bool        inDesktopEntry = false;

    while (std::getline(file, line)) {
        if (line == "[Desktop Entry]") {
            inDesktopEntry = true;
            continue;
        }
        if (line.starts_with("[") && line != "[Desktop Entry]") {
            inDesktopEntry = false;
            continue;
        }
        if (inDesktopEntry && line.starts_with("Icon=")) {
            return line.substr(5);
        }
    }

    return std::nullopt;
}

std::optional<std::string> CIconResolver::resolveIconFromTheme(const std::string& iconName, int size) {
    // Try hicolor theme (always available) and common themes
    std::vector<std::string> themes = {"hicolor"};

    for (const auto& theme : themes) {
        for (const auto& iconDir : ICON_DIRS) {
            if (iconDir.empty())
                continue;

            // Try preferred sizes first
            for (const auto& sizeStr : ICON_SIZES) {
                for (const auto& ext : ICON_EXTENSIONS) {
                    auto path = iconDir + "/" + theme + "/" + sizeStr + "/apps/" + iconName + ext;
                    if (fs::exists(path))
                        return path;
                }
            }
        }
    }

    return std::nullopt;
}
