# Java Overlay Studio

A Windows x64 desktop controller and native in-process overlay for Minecraft
1.8.9. The controller is written in C++20 with Qt 6/QML. It discovers Java
processes, then first uses the supported JVM Attach API to load a Qt-free
native JVMTI/JNI agent. If runtime Attach is unavailable, the controller can
fall back for a user-selected process with `jvm.dll` and a visible game window
to an ordinary Windows `LoadLibraryW` load, then invoke the DLL's exported
`McOverlay_Start` entry point.

The agent hooks `gdi32!SwapBuffers` (plus the optional
`opengl32!wglSwapLayerBuffers` path) and renders Dear ImGui through the LWJGL 2
OpenGL context. Because the UI is part of the game frame, borderless fullscreen
and F11 context/window recreation do not depend on desktop Z-order.

No Mod JAR is installed and no JAR is added to Minecraft's classpath. The
packaged `McOverlayAttachHelper.jar` is a controller-side command-line helper
only. The native fallback uses the normal Windows loader: it does not manually
map, unlink, conceal, or otherwise disguise the loaded module, and it contains
no anti-cheat bypass behavior.

## Components

- `src/ProcessScanner.*` enumerates `java.exe`/`javaw.exe`, resolves PID,
  executable path, working-set memory, title, and the best visible HWND.
- `src/OverlayManager.*` validates architecture, creates a per-session named
  pipe and 128-bit token, starts the Attach helper without a shell, and exposes
  the authenticated Agent/OpenGL state machine to QML.
- `attach-helper/` calls `VirtualMachine.attach()` and `loadAgentPath()`, then
  always detaches from the Attach transport.
- `native-loader/` is the x64 fallback for an Attach-disabled HotSpot. It uses
  `LoadLibraryW`, resolves the exported `McOverlay_Start` RVA, and calls that
  explicit initialization entry point in the target process.
- `agent/jvm.*` uses the `JavaVM*` supplied to `Agent_OnAttach`; the exported
  `JNI_GetCreatedJavaVMs` lookup is retained as a fallback for an explicitly
  invoked ordinary DLL entry point. `Agent_OnLoad` and `Agent_OnAttach` only
  copy bounded arguments and enqueue a `_beginthreadex` bootstrap, then return
  immediately. Every native worker attaches independently and owns a
  thread-local `JNIEnv*`.
- `agent/opengl_hook.*` installs and removes only the MinHook presentation
  hooks; it no longer intercepts `glLoadIdentity` or samples the already-reset
  OpenGL state at presentation time.
- `agent/overlay_renderer.*` owns Dear ImGui, the OpenGL2 backend, HWND/HGLRC
  generation tracking, WndProc chaining, and the animated Click GUI. Its sole
  default bind is single quote; the bind can be changed from Qt or ImGui. The
  old diagnostic status window has been removed. The Click GUI releases and
  continuously maintains LWJGL mouse grabbing, uses a lightweight framebuffer
  blur/dim layer, and provides four stable `1.0x`/`1.25x`/`1.5x`/`1.75x` size
  presets rather than a continuously mutating style slider. ESC closes the
  Click GUI, bind capture ignores the mouse click which opened it, and four
  independently rasterized Segoe UI fonts keep every preset sharp. Feature
  changes produce bounded, stacked, self-expiring notifications at the lower
  right of the game frame.
  WndProc capture is optional: clients which present on a
  thread other than the HWND owner still render normally and use a non-blocking
  physical-key/mouse polling fallback.
- `agent/bindings/` contains a validated `MappingDictionary`, pluggable
  `MappingProvider`, and freeze-on-resolve `MappingRegistry`. It resolves
  Minecraft 1.8.9 classes through JVMTI and the actual Minecraft class loader,
  then caches global class references plus JNI method/field IDs. Binding
  discovery is independent from ImGui startup: an unsupported transformed
  client can still display the in-game overlay.

## Controller dashboard and IPC

After an authenticated agent connection, the navigation rail animates from the
two pre-attach destinations (`Scanner` and `Settings`) to the attached-session
destinations (`Main`, `Player Status`, `ESP`, `Hypixel`, `About`, and
`Settings`). `Player Status` combines live IPC health/position data with a
drag-to-rotate, wheel-to-zoom Qt Quick 3D skin model resolved from the local
Minecraft name. `Browse processes`
keeps the current authenticated session alive; selecting its card returns to
the session, while attaching a different PID performs an explicit graceful
switch. Attach errors remain on `Main` with their structured error code,
details, and Retry action. The rail width is user-adjustable and clamped to a
responsive range. Rail width, the configurable Click-GUI key, and the selected
S/M/L/XL in-game scale are saved immediately with `QSettings`; the Hypixel key
uses its separate DPAPI-protected store. Skin PNGs are downloaded from the
trusted Mojang texture host, validated as 64-pixel Minecraft textures, and
atomically cached as local files before Quick3D renders them.

The Qt controller and in-game Click GUI share one authenticated, version-locked
`FEATURE_STATE` snapshot, so changes made in either surface are reflected by
the other and restored on the next attach. The Player ESP page exposes teammate
boxes and teammate arrows independently. The SAFE category contains an optional
Safewalk implementation which samples the player's support footprint through
JNI, drives Minecraft's own sneak `KeyBinding` at an edge, preserves a physical
sneak press, and releases only after a real air-to-block placement transition
plus the configured delay. Fly, BHop, and Scaffold are explicitly local/testing
movement tools: every off-to-on transition produces a ban-risk warning. The
Agent reads `Minecraft.getCurrentServerData().serverIP` and force-disables all
three on `hypixel.net` (including subdomains and explicit ports) unless the
separately persisted **Interface / Server Safety** override is enabled. This is
a safety interlock only; the project contains no anti-cheat bypass. Manual
Hypixel player-name requests travel from the game panel to the controller;
only the controller performs HTTPS and returns a bounded result snapshot to
ImGui. `BIND`/`BIND_CHANGED` keep the configurable Click-GUI hotkey synchronized.

The Agent reports `MATCH_STATE`, `PLAYER_STATUS`, and stable `PLAYER_FOUND
<name> <team-colour>` roster changes. Automatic network work starts only when
at least two distinct Bed Wars team tags are present, and is cancelled and
cleared outside the match. The controller resolves names, deduplicates requests,
queries asynchronously, and returns bounded `STATS` records. The independent
in-game table keeps one player per row, groups rows by scoreboard colour, and
colour-codes FKDR while keeping every network operation outside `SwapBuffers`.

The render callback publishes a small immutable game snapshot to a dedicated
telemetry thread. That thread, not `SwapBuffers`, performs named-pipe writes at
up to 10 Hz. The controller accepts monotonic, authenticated `GAME_STATE`
frames, validates every numeric range, ignores reordered samples, and marks a
dashboard sample stale from local receipt time. Mapping resolution failures
still produce `valid=0` telemetry so the UI can explain the unsupported profile
without blocking overlay rendering.

## Hypixel Bed Wars statistics

The controller accepts a Minecraft player name, resolves it to the UUID
required by Hypixel, then asynchronously calls the official `/v2/player` API
and reads the Bed Wars aggregate fields from `player.stats.Bedwars`. Paste a
key belonging to your registered developer application into **Settings →
Hypixel API key → Save securely**. It is encrypted with Windows DPAPI for the
current Windows account and the clear text is never exposed as a QML property,
logged, or written to disk. `HYPIXEL_API_KEY` remains available as a
non-persistent process-environment override for development:

```powershell
$env:HYPIXEL_API_KEY = 'your-registered-application-key'
.\MinecraftOverlayManager.exe
```

Lookups have a ten-second timeout, a bounded response size, rate-limit header
reporting, and a six-hour in-memory cache. Manual queries use that cache.
Automatic Agent roster discovery uses a six-hour result cache, a separate
ten-minute in-match de-duplication state machine, and a serial request queue.
Saving or replacing the key during an active match resumes the retained current
roster without waiting for another scoreboard change.

Hypixel queries have their own controller page and are not mixed with local
world state. Native ESP is independently guarded by
`Minecraft.isSingleplayer()` and publishes no bed/entity markers for remote
multiplayer worlds.

## Minecraft 1.8.9 data snapshot

Normal Forge 11.15.x release runtimes use stable SRG symbols; pure Vanilla
1.8.9 uses the embedded Notch-obfuscated profile. The read-only snapshot
currently exposes:

- player health/max health, entity ID, XYZ position, and collision AABB;
- the loaded entity count;
- 20 Hz living-entity AABB snapshots in an integrated single-player world,
  smoothed at render rate with `renderPartialTicks` and one-tick wrap-aware
  velocity extrapolation;
- physical two-block bed footprints for loaded chunks, deduplicated by the
  1.8.9 `BlockBed` HEAD metadata bit and expanded through its facing metadata.

Game/entity sampling runs on Minecraft's Java-owned render/main thread at no
more than 20 Hz; pipe telemetry remains capped at 10 Hz. Forge's splash
SwapBuffers calls are rejected with
`Minecraft.func_152345_ab()`. Object references for Minecraft, world, player,
entities, and block state are local to one sample; only classes and IDs are
cached. A separate daemon snapshots `ChunkProviderClient.chunkListing` every
500 ms, diffs signed chunk coordinates, and scans only new chunks. Each
`ExtendedBlockStorage` section is copied in one `GetCharArrayRegion` call (the
actual 1.8.9 representation is `char[4096]`), then decoded in native memory.
Unloaded chunks are removed immediately and cached bed heads receive a 1 Hz
single-point lazy validation. The Controller and Click GUI both expose an
immediate refresh command which
wakes the daemon through an auto-reset event and rebuilds all currently loaded
chunks, allowing beds placed after initial chunk load to appear without busy
polling. The scanner publishes a fixed-capacity cache;
`SwapBuffers` never scans blocks. On each rendered frame the JNI binding layer
copies `ActiveRenderInfo`'s direct MODELVIEW, PROJECTION, and VIEWPORT buffers
and reads `RenderManager.renderPosX/Y/Z`. W2S first converts absolute AABB
coordinates into Minecraft's camera-relative world space, then applies the
column-major model-view and projection matrices and performs the perspective
divide. Consequently yaw, pitch, dynamic FOV, hurt-camera effects, and view
bobbing are represented by the exact matrices that Minecraft used for the 3D
world rather than by the later HUD matrix visible at `SwapBuffers`.

Mapping discovery runs on a dedicated CRT thread attached with
`AttachCurrentThreadAsDaemon`; `GetLoadedClasses`, class-loader calls, and all
method/field-ID lookups are never executed by `SwapBuffers`. It takes exactly
one JVMTI loaded-class snapshot and reuses the matching Minecraft class loader
for every dependency. HotSpot may globally safepoint for that API, so an
unsupported/transformed client is not periodically rescanned: failure publishes
an unsupported state and permanently stops the resolver while the native
OpenGL overlay remains active. A completed immutable JNI cache is
release-published only after every reference and ID is valid; the render thread
acquire-loads it.

Forge build numbers do not normally change vanilla SRG names. A coremod that
removes or changes required members is detected as an unsupported binding
profile; the agent does not guess offsets or write JVM memory.

Lunar markers are detected independently from the symbol namespace. Lunar
1.8.9 releases that retain the canonical Notch namespace reuse the verified
Vanilla 1.8.9 dictionary under a dedicated high-priority Lunar provider.
Transformed builds still fail closed instead of guessing symbols. Additional
client/version packs are converted to owned `MappingDictionary` values,
strictly validated, registered before the registry freezes, and considered in
the same one-shot class snapshot. The resolver continues past compatibility
wrapper anchors that do not have a dictionary for the detected client family.

## Requirements

- Windows 10/11 x64
- CMake 3.24+
- Ninja or another CMake-supported Windows generator
- Qt 6.5+ with Core, Gui, Qml, Quick, QuickControls2, Quick3D, and Network
- a full x64 JDK (JDK 8 with `tools.jar`, or JDK 9+ with `jdk.attach`)
- an x64 Minecraft JVM

The first configure can fetch the pinned MinHook and Dear ImGui sources into
the build tree. For offline/reproducible builds, set
`MC_AGENT_MINHOOK_SOURCE_DIR` and `MC_AGENT_IMGUI_SOURCE_DIR` to vetted local
checkouts and disable `MC_OVERLAY_FETCH_AGENT_DEPENDENCIES`.

## Debug build with Qt MinGW

```powershell
cmake -S . -B build-native-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.1/mingw_64" `
  -DCMAKE_C_COMPILER="D:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe"

cmake --build build-native-debug --parallel
```

The build tree contains:

```text
build-native-debug/
  MinecraftOverlayManager.exe
  agent/McOverlayAgent.dll
  tools/McOverlayAttachHelper.jar
  tools/McOverlayNativeLoader.exe
```

The root build copies the current DLL and JAR on every build, including when
only the agent or helper changed.

## Validation

`tests/Run-AgentAttachSmoke.ps1` starts its own private Java child process and
attaches only to that PID. It verifies AttachHelper exit code 0 plus the
authenticated `HELLO` and `HOOK_READY OpenGL` messages. The dummy target has no
OpenGL context, so `RENDERER_READY` is intentionally tested only with a real
Minecraft client.

`tests/Run-OpenGlJvmSmoke.ps1` uses an owned synthetic JVM/WGL target. In
addition to renderer and input lifecycle checks, it requires a correctly framed
15-field `GAME_STATE valid=0` message when Minecraft mappings are absent.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-AgentAttachSmoke.ps1 `
  -AgentDll .\build-native-debug\agent\McOverlayAgent.dll `
  -AttachHelperJar .\build-native-debug\attach-helper\McOverlayAttachHelper.jar `
  -JavaHome 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot'
```

## Runtime notes

- Controller and target must run as the same Windows user and integrity level.
- A 32-bit/ARM64 JVM is rejected before attach; this build is x64 only.
- `-XX:+DisableAttachMechanism` prevents the official Attach API and cannot be
  reversed in a running JVM. The controller automatically tries its transparent
  Windows loader fallback; process mitigation or security software may still
  reject that operation. Start-time `-agentpath:` remains the JVM-supported
  alternative.
- `DETACH` disables input, rendering, hooks, and IPC. The agent DLL remains
  normally resident until JVM exit; forcibly calling `FreeLibrary` while JVM
  callbacks or OpenGL code may be active is intentionally avoided. The
  controller waits for `DETACH_COMPLETE` (or a peer close) before releasing the
  session and force-closes only after a 2.5-second confirmation timeout.
- A resident agent can establish a fresh authenticated session through another
  `Agent_OnAttach`/`McOverlay_Start`, but its DLL image is not hot-reloaded.
  Restart the target JVM before testing a newly built DLL.
- On HWND/HGLRC replacement (including F11), ImGui's Win32/OpenGL state is
  restored or rebuilt on the render thread before drawing resumes.
