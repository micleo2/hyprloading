# hyprloading - Startup Notification Plugin for Hyprland

## Overview

A Hyprland plugin that shows a small application icon bouncing next to the cursor
when an app is launching, similar to KDE Plasma's startup feedback. The icon
appears when the launcher (fuzzel) initiates a launch and disappears when the
application's window maps.

## How It Works (High Level)

```
fuzzel launches app
    │
    ├─ fuzzel calls xdg-activation-v1::get_activation_token(app_id=...)
    │  fuzzel calls commit()
    │
    ▼
hyprloading intercepts token creation
    │
    ├─ extracts app_id from the token
    ├─ resolves app_id → .desktop file → Icon= → icon file path
    ├─ loads icon as GL texture (cached)
    ├─ starts bounce animation near cursor
    │
    ▼
app window maps (openWindow event)
    │
    ├─ match window app_id against pending launch app_id
    ├─ fade out and remove the animation
    │
    ▼
done
```

## Detection: How We Know An App Is Launching

### Primary: xdg-activation-v1 token interception

Fuzzel (since v1.9.0) requests an activation token from the compositor before
spawning a child process. Hyprland handles this in its own protocol
implementation (not wlroots') at `src/protocols/XDGActivation.cpp`.

The flow inside Hyprland:

1. Client calls `get_activation_token` → `CXDGActivationProtocol::onGetToken()`
   creates a `CXDGActivationToken` object
2. Client calls `set_app_id(app_id)` on the token
3. Client calls `commit()` → token handler calls
   `g_pTokenManager->registerNewToken()`, sends done event, and pushes to
   `PROTO::activation->m_sentTokens`

**Hook point:** We hook `CXDGActivationProtocol::onGetToken` (or the commit
path) using `HyprlandAPI::findFunctionsByName()` +
`HyprlandAPI::createFunctionHook()`. After the original runs, we inspect
`PROTO::activation->m_sentTokens` to find the newly created token and its
`app_id`.

Alternatively, we can hook `g_pTokenManager->registerNewToken` which is called
from both the activation protocol and the exec dispatcher. However, the
activation path is more useful because it carries the `app_id`.

### Secondary: exec dispatcher hook

For apps launched via Hyprland's own `exec` dispatcher (keybinds, hyprctl), we
hook `CKeybindManager::spawnRawProc` to capture the command string and child
PID. The exec dispatcher does NOT set `XDG_ACTIVATION_TOKEN` today, so we
won't get an app_id from the protocol — instead, we attempt to match the
command to a .desktop file by parsing the Exec= field.

This is a nice-to-have and can be added in a second phase.

## Detection: How We Know The App Has Finished Launching

### Window map matching

Listen to `Event::bus()->m_events.window.open` (the `openWindow` event). When a
new window maps, compare its `app_id` against all pending launch entries:

```cpp
static auto listener = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
    auto appId = w->m_szInitialClass; // or w->m_szClass
    g_pHyprloading->onWindowOpen(appId, w);
});
```

Matching strategy (in order):
1. **Exact app_id match** — token's `app_id` == window's `app_id`
2. **Case-insensitive match** — some apps differ in casing
3. **Basename match** — strip `org.mozilla.` prefix style app_ids

### Timeout

If no matching window appears within **30 seconds**, cancel the animation.
This handles crashed apps or apps that don't map windows.

### Activation match (bonus)

If the launched app calls `xdg_activation_v1::activate(token, surface)`, we can
match the token string directly. This is the most reliable match but not all
apps do this.

## Icon Resolution Pipeline

```
app_id ("firefox")
    │
    ▼
.desktop file lookup
    │  scan: ~/.local/share/applications/
    │        /usr/share/applications/
    │        flatpak/snap export dirs
    │  match: filename stem == app_id (case-insensitive)
    │         or StartupWMClass == app_id
    │
    ▼
Icon= value ("firefox" or "/absolute/path.png")
    │
    ├─ if absolute path → use directly
    │
    ▼
icon theme resolution (freedesktop spec)
    │  themes: [user theme from gtk settings] → inherited → hicolor
    │  sizes:  48x48 preferred, then nearest larger, then scalable
    │  dirs:   ~/.local/share/icons/, /usr/share/icons/
    │  subdirs: apps/
    │  extensions: .png preferred, .svg fallback
    │  final fallback: /usr/share/pixmaps/{name}.png
    │
    ▼
file path ("/usr/share/icons/hicolor/48x48/apps/firefox.png")
    │
    ▼
load via cairo_image_surface_create_from_png()
    │  (or Hyprgraphics::CImage for SVG support)
    │
    ▼
upload to GL texture (CTexture)
    │  allocate → bind → set filters → R↔B swizzle → glTexImage2D
    │
    ▼
SP<CTexture> — cached by app_id for reuse
```

**No new dependencies.** Cairo and hyprgraphics are already linked by Hyprland.

## Animation

### Bounce animation (KDE-inspired, simplified)

The icon bounces vertically next to the cursor on a loop:

- **Position:** cursor position + offset (16px right, 16px down from hotspot)
- **Icon size:** 32x32 logical pixels (scaled by monitor scale)
- **Bounce cycle:** 600ms period (20 frames at 30ms each)
- **Vertical offset:** sinusoidal, amplitude ~10px
- **Fade in:** 150ms ease-in on appearance
- **Fade out:** 200ms ease-out when window maps (don't just pop away)

```
Frame offsets (simplified sine bounce, pixels):
  0ms:   y=0
  75ms:  y=-7
  150ms: y=-10  (peak)
  225ms: y=-7
  300ms: y=0    (bottom)
  375ms: y=-7
  450ms: y=-10  (peak)
  525ms: y=-7
  600ms: y=0    (loop)
```

### Animation driver

Use a `wl_event_loop_add_timer` on `g_pCompositor->m_wlEventLoop` (same
pattern as hyprtrails). The timer fires at the monitor's refresh rate and:

1. Updates animation progress for all active launch entries
2. Damages the icon's bounding box (old position + new position)
3. Schedules a frame for affected monitors

When no launches are pending, the timer is stopped to avoid wasting CPU.

## Rendering

### Render stage hook

We render at `RENDER_LAST_MOMENT` so the icon appears on top of everything
including the cursor:

```cpp
auto renderListener = Event::bus()->m_events.render.stage.listen(
    [](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT)
            return;
        g_pHyprloading->render(g_pHyprOpenGL->m_renderData.pMonitor.lock());
    }
);
```

### Drawing

For each active launch animation, add a `CTexPassElement` to the render pass:

```cpp
void Hyprloading::render(PHLMONITOR mon) {
    for (auto& launch : m_pendingLaunches) {
        Vector2D cursorPos = g_pPointerManager->position();
        // Convert to monitor-local coordinates
        CBox iconBox = {
            cursorPos.x - mon->m_position.x + ICON_OFFSET_X,
            cursorPos.y - mon->m_position.y + ICON_OFFSET_Y + launch.bounceOffset,
            ICON_SIZE, ICON_SIZE
        };
        iconBox.scale(mon->m_scale);

        CTexPassElement::SRenderData data;
        data.tex = launch.texture;
        data.box = iconBox;
        data.a   = launch.opacity;
        g_pHyprRenderer->m_renderPass.add(
            makeUnique<CTexPassElement>(std::move(data)));
    }
}
```

### Damage management

Each tick, damage the union of the icon's previous and current bounding boxes:

```cpp
CBox damagebox = lastIconBox.copy().expand(2);  // small margin
damagebox = damagebox.combine(currentIconBox.copy().expand(2));
g_pHyprRenderer->damageBox(damagebox);
g_pCompositor->scheduleFrameForMonitor(mon);
```

## Configuration

Exposed via Hyprland config (`hyprland.conf`):

```
plugin:hyprloading {
    # Enable/disable the plugin
    enabled = true

    # Icon size in logical pixels
    icon_size = 32

    # Timeout in milliseconds before giving up
    timeout = 30000

    # Bounce animation amplitude in pixels
    bounce_height = 10

    # Bounce cycle duration in milliseconds
    bounce_period = 600

    # Offset from cursor (x, y)
    icon_offset = 16, 16
}
```

Registered via `HyprlandAPI::addConfigValue()` in `PLUGIN_INIT`.

## File Structure

```
hyprloading/
├── DESIGN.md               # this file
├── Makefile                 # build system
├── hyprpm.toml              # hyprpm package manifest
├── compile_flags.txt        # clangd support
│
├── src/
│   ├── globals.hpp          # PHANDLE, global state pointer
│   ├── main.cpp             # PLUGIN_INIT, PLUGIN_EXIT, PLUGIN_API_VERSION
│   ├── Hyprloading.hpp      # main plugin class
│   ├── Hyprloading.cpp      # orchestration: hooks, events, animation loop
│   ├── IconResolver.hpp     # .desktop file + icon theme resolution
│   ├── IconResolver.cpp
│   ├── IconCache.hpp        # GL texture cache keyed by app_id
│   ├── IconCache.cpp
│   └── LaunchEntry.hpp      # per-launch state (app_id, texture, animation)
```

## Component Design

### `LaunchEntry` — Per-Launch State

```cpp
struct LaunchEntry {
    std::string                          appId;
    SP<CTexture>                         texture;
    std::chrono::steady_clock::time_point startTime;
    float                                opacity    = 0.f;  // 0→1 fade in
    float                                bounceOffset = 0.f;
    bool                                 fadingOut  = false;
    CBox                                 lastBox;           // for damage
};
```

### `IconResolver` — Desktop File + Icon Lookup

Stateless utility. Two main functions:

- `resolveDesktopFile(appId) → optional<DesktopEntry>` — scans XDG dirs for
  matching .desktop file, parses `Icon=` and `StartupNotify=`
- `resolveIconPath(iconName, size) → optional<string>` — freedesktop icon
  theme lookup returning a file path

Caches the mapping from app_id → icon file path in an `unordered_map` so
repeated launches of the same app don't re-scan the filesystem.

### `IconCache` — GL Texture Management

- `getTexture(iconPath) → SP<CTexture>` — loads and caches textures
- Textures are loaded lazily on first request
- Uses `cairo_image_surface_create_from_png()` for PNG,
  `Hyprgraphics::CImage` for SVG
- Uploads to GL via the allocate/bind/swizzle/glTexImage2D pattern
- Cache is keyed by file path (not app_id) so different apps sharing an icon
  share the texture

### `Hyprloading` — Main Plugin Class

Owns everything. Singleton accessed via `g_pHyprloading`.

**Responsibilities:**
- Registers config values, event listeners, and function hooks in init()
- Maintains `std::vector<LaunchEntry> m_pendingLaunches`
- Runs the animation timer
- Renders active animations
- Matches incoming windows to pending launches
- Handles timeouts and cleanup

**Event listeners:**
- `window.open` — match and complete pending launches
- `render.stage` — render at `RENDER_LAST_MOMENT`
- `input.mouse.move` — damage icon region when cursor moves (so it tracks)

**Function hooks:**
- `CXDGActivationProtocol::onGetToken` (or the commit handler) — detect new
  activation tokens with app_id

**Timer:**
- `wl_event_loop_add_timer` on `g_pCompositor->m_wlEventLoop`
- Fires at refresh rate when animations are active, stopped otherwise

## Build System

### Makefile

```makefile
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-c++11-narrowing
INCLUDES = $(shell pkg-config --cflags pixman-1 libdrm hyprland \
           pangocairo libinput libudev wayland-server xkbcommon)
LIBS = $(shell pkg-config --libs cairo)

SRC = $(wildcard src/*.cpp)
TARGET = hyprloading.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
```

### hyprpm.toml

```toml
[repository]
name = "hyprloading"
authors = ["mal"]

[hyprloading]
description = "Startup notification with bouncing app icon near cursor"
authors = ["mal"]
output = "hyprloading.so"
build = ["make all"]
```

## Implementation Plan

### Phase 1: Skeleton + Token Detection
1. Set up project structure, Makefile, globals, plugin entry points
2. Register config values
3. Hook into xdg-activation token creation to detect launches
4. Log detected app_ids to verify the hook works
5. Listen for `window.open` and match against pending app_ids
6. Log successful matches to verify detection pipeline

### Phase 2: Icon Resolution
7. Implement .desktop file scanner
8. Implement freedesktop icon theme resolver
9. Test: given an app_id, print the resolved icon file path

### Phase 3: Texture Loading
10. Implement Cairo PNG loading → GL texture upload
11. Add SVG support via hyprgraphics (fallback)
12. Implement texture cache
13. Test: load a known icon and verify texture dimensions

### Phase 4: Rendering + Animation
14. Implement the render stage hook at `RENDER_LAST_MOMENT`
15. Render a static icon next to the cursor for a pending launch
16. Add the animation timer (wl_event_loop_add_timer)
17. Implement bounce animation (sinusoidal vertical offset)
18. Implement fade-in on launch detection
19. Implement fade-out on window map match
20. Damage management (track cursor movement, damage old+new regions)

### Phase 5: Polish
21. Handle edge cases: app crashes (timeout), multiple simultaneous launches,
    monitor hotplug, cursor on different monitor than icon
22. Config value support (icon_size, timeout, bounce params)
23. Fallback icon for apps with no resolvable icon
24. Write hyprpm.toml for distribution
25. Test with fuzzel + various apps (Firefox, Kitty, Steam, Flatpaks)

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Hyprland's internal API changes between versions | Version hash check in PLUGIN_INIT; pin commits in hyprpm.toml |
| Function hook signatures change | Use findFunctionsByName with fallback error message |
| App doesn't match (wrong app_id) | Case-insensitive matching + StartupWMClass + timeout fallback |
| Icon not found | Ship a generic "loading" fallback icon, or use a simple spinner shape drawn via Cairo |
| GL texture upload on wrong thread | All rendering is on the compositor's main thread; no threading issues |
| Multiple monitors at different scales | Scale icon box per-monitor in render(); cursor pos is in logical coords |
| Fuzzel doesn't send app_id in token | Fall back to matching by PID or by timing (first window after token) |
