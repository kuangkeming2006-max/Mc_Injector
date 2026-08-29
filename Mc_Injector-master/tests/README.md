# Native agent smoke tests

Three complementary scripts cover startup and rendering. Every test owns its
Java target process from creation through cleanup, exposes no PID parameter,
and never loads into an existing Minecraft or third-party process.

## Private JVM + OpenGL render path

`Run-OpenGlJvmSmoke.ps1` is the rendering integration test. It starts the
opt-in `McOverlayOpenGlJvmSmoke` child executable, which dynamically creates a
JVM from the selected JDK and a shown-but-off-screen Win32 640x360 WGL window.
The harness calls `Agent_OnAttach` directly in its own process; it never
enumerates processes and has no loader or target-PID parameter.

The default `unified` mode owns the HWND, HGLRC, message pump, and
`SwapBuffers` on the JVM main thread. `-SplitThreads` reproduces Lunar-style
ownership: a dedicated thread creates and pumps the HWND while the JVM main
thread owns the HGLRC and submits frames. The controller validates the reported
thread IDs are actually different before accepting `RENDERER_READY`.

The named-pipe controller requires the complete lifecycle:

1. exact authenticated `HELLO` and `HOOK_READY OpenGL`;
2. `STATE 1 1`, `STATE_APPLIED 1 1`, and `RENDERER_READY OpenGL`, proving an
   actual ImGui OpenGL2 frame completed;
3. a 15-field `GAME_STATE` frame with `valid=0`, proving telemetry remains
   available and correctly framed when the synthetic JVM has no Minecraft
   mappings;
4. in unified mode, posted the default single-quote Click-GUI bind with exact
   close/open `STATE_CHANGED` replies, proving the installed WndProc bridge
   received input (INSERT and right Shift intentionally have no implicit action);
5. continuous `SwapBuffers` during `DETACH`, followed by
   `DETACH_COMPLETE`, which covers render-thread ImGui cleanup, WndProc
   restoration, hook disable, and callback drain;
6. graceful `WM_CLOSE`, `DestroyJavaVM`, and child exit code `0`.

Build and run this test explicitly (it is excluded from normal builds):

```powershell
cmake --build .\build-debug --target McOverlayOpenGlJvmSmoke --parallel
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-OpenGlJvmSmoke.ps1 `
  -HarnessExe .\build-debug\McOverlayOpenGlJvmSmoke.exe `
  -AgentDll .\build-debug\McOverlayAgent.dll `
  -JavaHome 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot'
```

Run the split-owner topology with the same private executable:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-OpenGlJvmSmoke.ps1 `
  -HarnessExe .\build-debug\McOverlayOpenGlJvmSmoke.exe `
  -AgentDll .\build-debug\McOverlayAgent.dll `
  -JavaHome 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot' `
  -SplitThreads
```

The split test does not synthesize a global configured-hotkey event: posted
window messages intentionally do not alter `GetAsyncKeyState`, while
`SendInput` would affect the user's active desktop. It instead validates the
optional-WndProc renderer path, authenticated state transitions, and teardown.

The default end-to-end deadline is 45 seconds. The WGL window is marked
visible because the production renderer intentionally rejects hidden HWNDs,
but it is placed outside the virtual screen and never activates.

`McOverlayHotkeyCaptureTests` is a small Controller-side regression target. It
delivers native virtual-key and Escape events through Qt's application event
filter and verifies that one-shot binding capture succeeds without relying on
QML focus:

```powershell
cmake --build .\build-debug --target McOverlayHotkeyCaptureTests --parallel
.\build-debug\McOverlayHotkeyCaptureTests.exe
```

## JDK Attach path

`Run-AgentAttachSmoke.ps1` performs a narrow end-to-end check of the supported
JDK Attach path. It does **not** accept a target PID and never attaches to an
existing Minecraft or third-party process. The script:

1. compiles and starts `AttachSmokeTarget.java` in a private child JVM;
2. creates a unique Windows named-pipe server with a random 128-bit token;
3. invokes `McOverlayAttachHelper.jar`, which calls
   `VirtualMachine.loadAgentPath` for that child PID only;
4. authenticates `HELLO 1 <child-pid> <token>` and requires
   `HOOK_READY OpenGL`;
5. verifies that AttachHelper exits with code `0`, sends `STATE 0 0` and
   `DETACH`, then terminates the private JVM and removes its temporary classes.

The target initializes AWT so `gdi32.dll` is loaded, but creates no visible
window and no OpenGL context. Consequently `RENDERER_READY` is intentionally
not a success condition; that event can only be covered by a real rendering
integration test.

## Run manually

Build the x64 agent DLL and AttachHelper JAR first, then run from the repository
root in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-AgentAttachSmoke.ps1 `
  -AgentDll .\build-debug\agent\McOverlayAgent.dll `
  -AttachHelperJar .\build-debug\attach-helper\McOverlayAttachHelper.jar
```

If the build uses different output directories, pass their absolute or
repository-relative paths instead. Set `-JavaHome` when `java.exe` and
`javac.exe` are not both discoverable from `JAVA_HOME`/`PATH`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-AgentAttachSmoke.ps1 `
  -AgentDll D:\path\to\McOverlayAgent.dll `
  -AttachHelperJar D:\path\to\McOverlayAttachHelper.jar `
  -JavaHome 'C:\Program Files\Microsoft\jdk-21.0.10.7-hotspot'
```

The default protocol deadline is 30 seconds and may be changed with
`-TimeoutSeconds`. A pass returns process exit code `0`; validation, compile,
attach, authentication, timeout, or helper failures return `1` after cleanup.

## Native loader fallback

`Run-NativeLoaderSmoke.ps1` validates the Windows native-loader fallback used
when HotSpot rejects JDK Attach. It starts its private JVM with
`-XX:+DisableAttachMechanism`, then:

1. creates a unique named pipe and random 128-bit authentication token;
2. runs `McOverlayNativeLoader.exe` with only the owned child PID, agent DLL,
   and agent options;
3. requires native-loader exit code `0` and exact
   `HELLO 1 <child-pid> <token>` plus `HOOK_READY OpenGL` messages;
4. sends `STATE 0 0` and `DETACH`, then invokes the loader a second time against
   the still-running JVM and requires a second authenticated handshake and
   detach from the already resident exact-path DLL;
5. terminates the child and removes only its GUID-named temporary classes
   directory.

Run it after building the native loader and agent:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\Run-NativeLoaderSmoke.ps1 `
  -AgentDll .\build-debug\agent\McOverlayAgent.dll `
  -NativeLoader .\build-debug\native-loader\McOverlayNativeLoader.exe
```

Use `-JavaHome` to select a full JDK containing both `java.exe` and `javac.exe`,
and `-TimeoutSeconds` to override the 30-second protocol deadline. The test
target initializes AWT but creates no OpenGL context, so `RENDERER_READY` is
intentionally outside this test's success condition.

## Safety boundary

- The OpenGL harness creates the JVM and WGL window in its own child process;
  it contains no process enumeration, remote-loader call, or target PID input.
- No PID parameter exists: the attach PID comes directly from the child
  `System.Diagnostics.Process` created by the same script invocation.
- The native-loader test passes only the PID of its own
  `-XX:+DisableAttachMechanism` child JVM to `McOverlayNativeLoader.exe`.
- Cleanup sends `DETACH`; the agent remains normally loaded until the private
  JVM exits, matching the production runtime's no-`FreeLibrary` rule.
- Each script only recursively deletes its own specifically prefixed,
  GUID-named directory directly beneath the current Windows temporary root.
- The native-loader test uses the same explicit `CreateRemoteThread` calls as
  production (`LoadLibraryW`, then `McOverlay_Start`) against only its own child
  JVM. It performs no manual mapping, concealment, or anti-cheat bypass.
