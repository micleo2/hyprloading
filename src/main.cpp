#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <vector>

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#undef private

#include <hyprland/src/render/pass/TexPassElement.hpp>

#include "globals.hpp"
#include "LaunchEntry.hpp"
#include "IconResolver.hpp"
#include "IconCache.hpp"

// ─── state ──────────────────────────────────────────────────────────────────
static std::vector<LaunchEntry> g_pendingLaunches;
static CIconResolver            g_iconResolver;
static CIconCache               g_iconCache;

// ─── window open → start animation ─────────────────────────────────────────
static void onWindowOpen(PHLWINDOW window) {
    static const auto PENABLED = CConfigValue<Hyprlang::INT>("plugin:hyprloading:enabled");
    if (!*PENABLED)
        return;

    std::string wClass = window->m_initialClass;
    if (wClass.empty())
        wClass = window->m_class;
    if (wClass.empty())
        return;

    logToFile(std::format("window.open: class='{}'", wClass));

    // If there's a pending launch for this class, mark it for fade-out
    for (auto& entry : g_pendingLaunches) {
        std::string eLower = entry.appId;
        std::string wLower = wClass;
        std::transform(eLower.begin(), eLower.end(), eLower.begin(), ::tolower);
        std::transform(wLower.begin(), wLower.end(), wLower.begin(), ::tolower);

        bool matched = (entry.appId == wClass) || (eLower == wLower);
        if (!matched) {
            auto dot = eLower.rfind('.');
            if (dot != std::string::npos && eLower.substr(dot + 1) == wLower)
                matched = true;
        }

        if (matched) {
            logToFile(std::format("match: '{}' already pending, fading out", wClass));
            entry.fadingOut = true;
            return;
        }
    }

    // New window — start a brief bouncing icon animation
    static const auto PICONSIZE = CConfigValue<Hyprlang::INT>("plugin:hyprloading:icon_size");
    auto              iconPath  = g_iconResolver.resolveIconPath(wClass, *PICONSIZE);

    SP<CTexture> texture = nullptr;
    if (iconPath) {
        texture = g_iconCache.getTexture(*iconPath);
        logToFile(std::format("icon: '{}' -> {}", wClass, *iconPath));
    } else {
        logToFile(std::format("icon: '{}' not found", wClass));
    }

    LaunchEntry entry;
    entry.appId     = wClass;
    entry.texture   = texture;
    entry.startTime = std::chrono::steady_clock::now();
    entry.opacity   = 0.f;
    // Auto fade-out after appearing (since the window is already here)
    entry.fadingOut = false;

    g_pendingLaunches.push_back(std::move(entry));
    logToFile(std::format("launch: started animation for '{}' (pending: {})", wClass, g_pendingLaunches.size()));

    // Damage to trigger first render
    for (auto& mon : g_pCompositor->m_monitors)
        g_pCompositor->scheduleFrameForMonitor(mon);
}

// ─── rendering + animation ──────────────────────────────────────────────────
static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT)
        return;
    if (g_pendingLaunches.empty())
        return;

    auto mon = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!mon)
        return;

    static const auto PICONSIZE     = CConfigValue<Hyprlang::INT>("plugin:hyprloading:icon_size");
    static const auto POFFSET_X     = CConfigValue<Hyprlang::INT>("plugin:hyprloading:icon_offset_x");
    static const auto POFFSET_Y     = CConfigValue<Hyprlang::INT>("plugin:hyprloading:icon_offset_y");
    static const auto PBOUNCEHEIGHT = CConfigValue<Hyprlang::INT>("plugin:hyprloading:bounce_height");
    static const auto PBOUNCEPERIOD = CConfigValue<Hyprlang::INT>("plugin:hyprloading:bounce_period");

    Vector2D cursorPos = g_pPointerManager->position();
    auto     now       = std::chrono::steady_clock::now();
    bool     needsDamage = false;

    // Show for 2 seconds then fade out
    static constexpr long SHOW_MS    = 2000;
    static constexpr long FADE_IN_MS = 150;
    static constexpr long FADE_OUT_MS = 400;

    for (size_t i = 0; i < g_pendingLaunches.size();) {
        auto& entry   = g_pendingLaunches[i];
        long  elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.startTime).count();

        // After SHOW_MS, start fading out
        if (!entry.fadingOut && elapsed > SHOW_MS)
            entry.fadingOut = true;

        // Fade in
        if (!entry.fadingOut) {
            entry.opacity = std::min(1.f, (float)elapsed / (float)FADE_IN_MS);
        } else {
            entry.opacity -= 1.f / ((float)FADE_OUT_MS / 16.f);
            if (entry.opacity <= 0.f) {
                logToFile(std::format("faded: '{}'", entry.appId));
                g_pendingLaunches.erase(g_pendingLaunches.begin() + i);
                continue;
            }
        }

        // Bounce
        float period    = (float)*PBOUNCEPERIOD;
        float amplitude = (float)*PBOUNCEHEIGHT;
        float phase     = std::fmod((float)elapsed, period) / period * 2.f * (float)M_PI;
        entry.bounceOffset = -amplitude * std::abs(std::sin(phase));

        // Render icon
        if (entry.texture) {
            int  iconSize = *PICONSIZE;
            CBox iconBox  = {
                cursorPos.x - mon->m_position.x + *POFFSET_X,
                cursorPos.y - mon->m_position.y + *POFFSET_Y + entry.bounceOffset,
                (double)iconSize,
                (double)iconSize,
            };
            iconBox.scale(mon->m_scale);

            CTexPassElement::SRenderData data;
            data.tex = entry.texture;
            data.box = iconBox;
            data.a   = entry.opacity;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));

            entry.lastBox = iconBox;
        }

        needsDamage = true;
        i++;
    }

    if (needsDamage) {
        for (auto& launch : g_pendingLaunches) {
            if (!launch.lastBox.empty()) {
                CBox dmg = launch.lastBox.copy().expand(20);
                g_pHyprRenderer->damageBox(dmg);
            }
        }
        g_pCompositor->scheduleFrameForMonitor(mon);
    }
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprloading] Version mismatch (headers != running hyprland)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprloading] Version mismatch");
    }

    logToFile("=== PLUGIN_INIT ===");

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:icon_size", Hyprlang::INT{32});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:bounce_height", Hyprlang::INT{10});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:bounce_period", Hyprlang::INT{600});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:icon_offset_x", Hyprlang::INT{16});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprloading:icon_offset_y", Hyprlang::INT{16});

    static auto P1 = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onWindowOpen(w); });
    static auto P2 = Event::bus()->m_events.render.stage.listen([&](eRenderStage stage) { onRenderStage(stage); });

    logToFile("PLUGIN_INIT complete");

    HyprlandAPI::addNotification(PHANDLE, "[hyprloading] Loaded successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 3000);

    return {"hyprloading", "Startup notification with bouncing app icon", "mal", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    logToFile("=== PLUGIN_EXIT ===");
    g_pendingLaunches.clear();
}
