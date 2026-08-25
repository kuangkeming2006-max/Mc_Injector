# McOverlayAgent (Minecraft 1.8.9 / LWJGL2)

`McOverlayAgent.dll` is an x64 Windows JVMTI agent. It is loaded through the
standard JVM Attach API (`VirtualMachine.loadAgentPath`) or at JVM startup with
`-agentpath`. It does not use manual mapping, hide its module, inject a Java
JAR, transform bytecode, or install a Forge mod.

`Agent_OnLoad` and `Agent_OnAttach` only validate/copy bounded input and queue
`_beginthreadex`, then return to the JVM immediately. The background bootstrap
attaches itself as a daemon before initializing JVMTI, hooks, and IPC. DLL
references and bootstrap thread handles are drained explicitly during unload.

The worker thread installs MinHook detours for `gdi32!SwapBuffers` and, when
available, `opengl32!wglSwapLayerBuffers`. Rendering happens immediately before
the original present call using Dear ImGui's Win32 + OpenGL2 backends, matching
Minecraft 1.8.9's LWJGL2 renderer. `DllMain` only disables thread callbacks.

## Building

Configure this directory independently, or include it from the root project:

```powershell
cmake -S agent -B build-agent -G Ninja `
  -DMC_AGENT_JDK_ROOT="C:/Program Files/Microsoft/jdk-21.0.10.7-hotspot" `
  -DMC_AGENT_MINHOOK_SOURCE_DIR="D:/deps/minhook" `
  -DMC_AGENT_IMGUI_SOURCE_DIR="D:/deps/imgui"
cmake --build build-agent --config Debug
```

Dependency fetching is opt-in (`MC_AGENT_FETCH_DEPENDENCIES=ON`) and pins
MinHook v1.3.4 and Dear ImGui v1.92.9. The produced DLL is intentionally x64;
it must match the target JVM architecture.

## Attach options and IPC

The required options string is semicolon-separated:

```text
pipe=\\.\pipe\McOverlay.<session>;token=<32 lowercase/uppercase hex>;protocol=1
```

The controller creates the named-pipe server first. The agent connects with
`CreateFileW`, gives up after 15 seconds, and sends:

```text
HELLO 1 <java-pid> <token>
HOOK_READY OpenGL
RENDERER_READY OpenGL
```

`RENDERER_READY` is emitted as soon as a valid Minecraft HWND/HGLRC initializes
ImGui; Minecraft field mappings are not a prerequisite. The controller sends
`STATE <visible> <interactive>`, `FEATURE_STATE <seven flags> <bed-radius-3..10>`, `BIND
<virtual-key>`, bounded `HYPIXEL_RESULT`/`STATS` snapshots, or `DETACH`; state
commands are acknowledged with `STATE_APPLIED <visible> <interactive>`. Roster
changes travel in the other direction as `MATCH_STATE <0|1>`, percent-encoded
`PLAYER_STATUS <local-name>`, and `PLAYER_FOUND <name> <scoreboard-colour>`
records. `PLAYER_FOUND` is emitted only while the Bed Wars team-tag detector is
active.
`DETACH` restores hooks/WndProc and stops the runtime, emits
`DETACH_COMPLETE`, but deliberately does not call `FreeLibrary`. The resident
image can accept a later authenticated session through either `Agent_OnAttach`
or `McOverlay_Start`; the native loader resolves the existing exact-path module
and re-enters its startup export instead of reporting a duplicate injection.
Testing different DLL bytes still requires a target-JVM restart.

One local Click-GUI hotkey is available after renderer initialization. It
defaults to `'` (single quote), can be rebound from the controller or Click GUI,
and opens/closes interactive capture. INSERT and right Shift have no implicit
action. ESC closes an open GUI, and the binding capture waits for the opening
mouse click to be released and accepts keyboard keys only. Normal 1.8.9 clients
use `func_71364_i` / `func_71381_h`; transformed
clients additionally keep `org.lwjgl.input.Mouse.setGrabbed(false)` asserted
while the GUI is open.

The Click GUI includes exactly four size presets (`1.0x`, `1.25x`, `1.5x`, and
`1.75x`). Its geometry eases between immutable base-style values, avoiding the
old compounded `ScaleAllSizes` crash. Four pre-rasterized Segoe UI fonts are
selected only at a safe pre-frame boundary instead of scaling one blurry
bitmap. Rounded borderless surfaces, eased transitions, a framebuffer blur/dim
layer, stacked lower-right feature toasts, and the draggable team-grouped
one-player-per-row table replace the previous always-visible diagnostic window.

Local changes are reported as `STATE_CHANGED <visible> <interactive>` so the Qt
controller remains authoritative and synchronized. `STATE_CHANGED`,
`RENDERER_READY`, and the 10 Hz `GAME_STATE` snapshot are published from the
render callback into fixed-size mailboxes. A background thread performs all
formatting and named-pipe writes, so a slow controller cannot stall
`SwapBuffers`.

The same-process WndProc bridge is also supported when `SwapBuffers` executes
on a Lunar-style presentation thread different from the HWND owner. The window
thread performs only atomic hotkey/suppression dispatch; ImGui cursor/buttons
are polled by the OpenGL owner thread. Raw input is swallowed while Click GUI is
interactive. If the presentation thread is native, the agent attaches it to
the JVM once as a daemon so LWJGL mouse release and mapped Minecraft focus calls
actually run; the grab is maintained every frame and full focus release is
reasserted at a bounded 10 Hz.

## Minecraft bindings

The read-only binding layer supports exactly Minecraft 1.8.9 through a
freeze-on-resolve `MappingRegistry`:

- Forge runtime SRG class/member names, independent of a specific Forge build.
- Vanilla Notch names from the MCP 1.8.9 joined mapping.
- Lunar environment markers with two validated 1.8.9 profiles: legacy Notch
  symbols and current MCP-named classes/members. The named profile uses
  Lunar's static `theMinecraft` and `loadedEntityList` fields rather than
  Forge SRG names; future transformed releases can still register an exact
  dictionary through `GameBindings::registerMappingDictionary()` before
  resolver freeze.

Resolution runs on an independent CRT thread attached with
`AttachCurrentThreadAsDaemon`. It takes exactly one JVMTI `GetLoadedClasses`
snapshot, and every dependent class must match the actual Minecraft classloader.
HotSpot may globally safepoint for that API, so an unsupported/transformed
client is not periodically rescanned and the resolver exits permanently with an
unsupported state. It builds an immutable cache privately and release-publishes
it only after every global class reference and method/field ID is valid.
`SwapBuffers` only acquire-checks that publication; it never performs class
discovery and an unsupported mapping never blocks ImGui rendering. No `JNIEnv*`
or local `jobject` crosses a frame/thread boundary. A detached native GL thread
is attached once as a daemon and keeps a thread-local `JNIEnv*` only for its own
callback lifetime.
`func_152345_ab` rejects Forge splash and non-game render contexts.

Player health, entity ID, position, AABB and loaded-entity count are sampled at
no more than 20 Hz while IPC telemetry remains capped at 10 Hz. In an integrated
single-player world, fixed-capacity ESP snapshots collect living-entity AABBs;
render-time partial-tick interpolation and wrap-aware one-tick extrapolation
keep moving boxes smooth without increasing JNI enumeration frequency. A
dedicated JNI daemon diffs the loaded
chunk set every 500 ms, copies each new `ExtendedBlockStorage` `char[4096]`
with `GetCharArrayRegion`, decodes Bed block-state IDs in C++, removes unloaded
chunks, and lazily revalidates cached beds once per second. Bed HEAD/facing
metadata removes double-counting and expands each marker to its full two-block
footprint. The same bulk pass retains a sparse palette of common defense
materials and precomputes exact 1–10 block distance rings at or above bed Y.
The renderer uses those rings for fixed-pixel inventory-style panels and never
issues a block JNI call. Player-only bed proximity transitions feed the existing
rate-limited lower-right toast stack. Remote multiplayer worlds clear these marker arrays
inside the Agent. Every presentation frame reads the cached direct FloatBuffer
and IntBuffer objects owned by `ActiveRenderInfo` plus
`RenderManager.renderPosX/Y/Z` through JNI. The renderer subtracts that camera
origin, applies Minecraft's column-major MODELVIEW and PROJECTION matrices,
performs the perspective divide, and maps through the captured VIEWPORT. This
tracks camera yaw/pitch, FOV, view bobbing, and hurt-camera effects without a
second `glLoadIdentity` hook or reliance on the orthographic HUD state present
at `SwapBuffers`.

## Lifecycle limitations

- An external controller disconnect hides the UI and stops this runtime; the
  JVM keeps the normally loaded agent module until process shutdown.
- If an HGLRC is destroyed before cleanup, its OpenGL objects are driver-owned
  and reclaimed with that context. The code never issues `glDelete*` against a
  different context.
- This module targets OpenGL/LWJGL2 only. It is not a Vulkan or Direct3D hook.
