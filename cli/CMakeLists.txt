cmake_minimum_required(VERSION 3.24)

# The lightweight console injector. Pure C++/Win32 — it deliberately links no
# Qt module. The executable is a console-subsystem binary (double-clicking it
# opens the terminal REPL), statically links the MinGW runtime, and deploys
# the agent DLL, the Attach helper JAR, and the native loader fallback beside
# itself, so the whole runtime is: McInjectorLite.exe + agent/ + tools/.

if(NOT MC_OVERLAY_BUILD_CLI)
    return()
endif()

add_executable(McInjectorLite
    main.cpp
    ConsoleIo.h
    ConsoleIo.cpp
    Json.h
    Json.cpp
    ThreadQueue.h
    WinApiKeyStore.h
    WinApiKeyStore.cpp
    WinHttpClient.h
    WinHttpClient.cpp
    WinOverlayManager.h
    WinOverlayManager.cpp
    WinProcessScanner.h
    WinProcessScanner.cpp
    WinSettings.h
    WinSettings.cpp
    HypixelService.h
    HypixelService.cpp
    PlayerStatsService.h
    PlayerStatsService.cpp
)

target_compile_features(McInjectorLite PRIVATE cxx_std_20)
target_compile_definitions(McInjectorLite PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    UNICODE
    _UNICODE
)

target_link_libraries(McInjectorLite PRIVATE
    user32
    advapi32
    crypt32
    winhttp
    psapi
)

if(MSVC)
    target_compile_options(McInjectorLite PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(McInjectorLite PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    # Self-contained deployment: no libgcc/libstdc++/libwinpthread DLLs next to
    # the executable. msvcrt.dll itself is part of Windows.
    target_link_options(McInjectorLite PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++)
endif()

# Console subsystem on purpose; never the GUI subsystem.
set_target_properties(McInjectorLite PROPERTIES
    OUTPUT_NAME "McInjectorLite"
    WIN32_EXECUTABLE FALSE
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF)

# Deploy the exact runtime OverlayManager locates relative to the executable:
# agent/McOverlayAgent.dll, tools/McOverlayAttachHelper.jar, and
# tools/McOverlayNativeLoader.exe. No other files are required.
add_custom_command(TARGET McInjectorLite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:McInjectorLite>/agent"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:McInjectorLite>/tools"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:McOverlayAgent>"
            "$<TARGET_FILE_DIR:McInjectorLite>/agent/McOverlayAgent.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MC_OVERLAY_ATTACH_HELPER_JAR}"
            "$<TARGET_FILE_DIR:McInjectorLite>/tools/McOverlayAttachHelper.jar"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:McOverlayNativeLoader>"
            "$<TARGET_FILE_DIR:McInjectorLite>/tools/McOverlayNativeLoader.exe"
    DEPENDS
        McOverlayAgent
        McOverlayNativeLoader
        mc_overlay_attach_helper
    VERBATIM)
