# McOverlay JVM Attach Helper

`McOverlayAttachHelper.jar` is a deliberately small launcher for the supported
JDK Attach API. It attaches to an already-running JVM, calls
`VirtualMachine.loadAgentPath(...)`, and always detaches before exiting. It does
not use manual DLL mapping, remote-thread injection, module hiding, or any
anti-detection behavior.

This helper JAR runs **only as a controller-side CLI**. It is never added to the
Minecraft/Forge class path and no Java bridge JAR is loaded into Minecraft. The
only artifact loaded into the target JVM is the native DLL passed to
`loadAgentPath`; HotSpot invokes that DLL's `Agent_OnAttach(JavaVM*, char*,
void*)` entry point.

## Build

A full JDK is required; a JRE alone is insufficient. The source and emitted
class format are Java 8 compatible.

The helper runtime is independent of Minecraft's runtime. In particular, the
controller must not try to run this JAR with a launcher-bundled Java 8 **JRE**
that lacks `tools.jar`; it should locate or bundle a full JDK. A current HotSpot
JDK can ordinarily attach to a separate Java 8 HotSpot target through the Attach
protocol, but the controller should surface the helper's explicit failure code
if a particular vendor/version combination rejects cross-version attachment.

```powershell
cmake -S attach-helper -B build-attach-helper
cmake --build build-attach-helper
```

The resulting file is `build-attach-helper/McOverlayAttachHelper.jar`.

To include it from the application project, add this to the parent
`CMakeLists.txt`:

```cmake
add_subdirectory(attach-helper)
add_dependencies(MinecraftOverlayManager mc_overlay_attach_helper)

add_custom_command(TARGET MinecraftOverlayManager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MC_OVERLAY_ATTACH_HELPER_JAR}"
            "$<TARGET_FILE_DIR:MinecraftOverlayManager>/McOverlayAttachHelper.jar")
```

## Run

On JDK 9 or newer, explicitly resolve the Attach API module:

```powershell
java --add-modules jdk.attach -jar McOverlayAttachHelper.jar `
    12345 "C:\absolute\path\McOverlayAgent.dll" "pipe=McOverlay-12345"
```

On JDK 8, run with that JDK's `tools.jar` on the class path:

```powershell
java -cp "McOverlayAttachHelper.jar;$env:JAVA_HOME\lib\tools.jar" `
    com.mcoverlay.attach.AttachHelper `
    12345 "C:\absolute\path\McOverlayAgent.dll" "pipe=McOverlay-12345"
```

The helper must be run under a user/security context allowed to attach to the
target JVM. The native agent architecture must match the target JVM (normally
x64). Dynamic loading must also be enabled in the target JVM; recent JDKs may
warn or require the target to have been launched with
`-XX:+EnableDynamicAgentLoading`.

Agent options are optional. When supplied, they must be passed as one command
line argument; the helper forwards them unchanged to `Agent_OnAttach`.

## Exit codes

| Code | Meaning |
| ---: | --- |
| 0 | Agent loaded and the helper detached successfully |
| 2 | Invalid argument count |
| 3 | Invalid PID |
| 4 | Missing, relative, unreadable, or non-DLL agent path |
| 10 | Target does not support the Attach API |
| 11 | I/O failure while establishing the attach connection |
| 12 | JVM rejected or could not load the agent DLL |
| 13 | `Agent_OnAttach` returned an initialization error |
| 14 | Permission/security failure |
| 15 | Agent loaded, but detaching the Attach API connection failed |
| 20 | Unexpected runtime failure |

Error details are written to standard error. A successful load writes one line
to standard output, making it straightforward for the Qt controller to capture
and present the result.
