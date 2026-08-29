cmake_minimum_required(VERSION 3.24)

project(McOverlayAgent LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "McOverlayAgent is a Windows-only target.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "McOverlayAgent must be built as x64 to match the Minecraft JVM.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MC_AGENT_JDK_ROOT "" CACHE PATH
    "JDK root containing include/jni.h and include/jvmti.h")
set(MC_AGENT_MINHOOK_SOURCE_DIR "" CACHE PATH
    "Existing MinHook source directory")
set(MC_AGENT_IMGUI_SOURCE_DIR "" CACHE PATH
    "Existing Dear ImGui source directory")
option(MC_AGENT_FETCH_DEPENDENCIES
    "Fetch pinned MinHook and Dear ImGui sources while configuring" OFF)
option(MC_AGENT_STATIC_MINGW_RUNTIME
    "Link libgcc/libstdc++ statically into the agent when using MinGW" ON)

# The DLL uses the invocation interfaces exposed by the already-running JVM and
# therefore must not link jvm.lib. Only the JNI/JVMTI headers are required.
if(NOT MC_AGENT_JDK_ROOT AND DEFINED ENV{JAVA_HOME})
    set(MC_AGENT_JDK_ROOT "$ENV{JAVA_HOME}")
endif()

if(NOT MC_AGENT_JDK_ROOT)
    find_program(MC_AGENT_JAVAC_EXECUTABLE NAMES javac)
    if(MC_AGENT_JAVAC_EXECUTABLE)
        get_filename_component(_javac_bin "${MC_AGENT_JAVAC_EXECUTABLE}" DIRECTORY)
        get_filename_component(MC_AGENT_JDK_ROOT "${_javac_bin}" DIRECTORY)
    endif()
endif()

find_path(MC_AGENT_JNI_INCLUDE_DIR
    NAMES jni.h jvmti.h
    HINTS "${MC_AGENT_JDK_ROOT}/include"
    NO_DEFAULT_PATH)
find_path(MC_AGENT_JNI_PLATFORM_INCLUDE_DIR
    NAMES jni_md.h
    HINTS "${MC_AGENT_JDK_ROOT}/include/win32"
    NO_DEFAULT_PATH)

if(NOT MC_AGENT_JNI_INCLUDE_DIR OR NOT MC_AGENT_JNI_PLATFORM_INCLUDE_DIR)
    message(FATAL_ERROR
        "JNI/JVMTI headers were not found. Set MC_AGENT_JDK_ROOT to a 64-bit JDK.")
endif()

if(MC_AGENT_FETCH_DEPENDENCIES)
    include(FetchContent)

    if(NOT MC_AGENT_MINHOOK_SOURCE_DIR)
        FetchContent_Declare(minhook_external
            GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
            GIT_TAG v1.3.4
            GIT_SHALLOW TRUE
            # Populate the vetted sources without importing MinHook's install
            # rules into the controller's runtime package.
            SOURCE_SUBDIR _mc_overlay_no_subdirectory)
        FetchContent_MakeAvailable(minhook_external)
        set(MC_AGENT_MINHOOK_SOURCE_DIR "${minhook_external_SOURCE_DIR}")
    endif()

    if(NOT MC_AGENT_IMGUI_SOURCE_DIR)
        FetchContent_Declare(imgui_external
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.92.9
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(imgui_external)
        set(MC_AGENT_IMGUI_SOURCE_DIR "${imgui_external_SOURCE_DIR}")
    endif()
endif()

if(NOT MC_AGENT_MINHOOK_SOURCE_DIR OR
   NOT EXISTS "${MC_AGENT_MINHOOK_SOURCE_DIR}/include/MinHook.h")
    message(FATAL_ERROR
        "MinHook was not found. Set MC_AGENT_MINHOOK_SOURCE_DIR, or explicitly "
        "configure with -DMC_AGENT_FETCH_DEPENDENCIES=ON.")
endif()

if(TARGET MinHook::MinHook)
    set(MC_AGENT_MINHOOK_TARGET MinHook::MinHook)
elseif(TARGET minhook)
    set(MC_AGENT_MINHOOK_TARGET minhook)
else()
    add_library(mc_agent_minhook STATIC
        "${MC_AGENT_MINHOOK_SOURCE_DIR}/src/buffer.c"
        "${MC_AGENT_MINHOOK_SOURCE_DIR}/src/hook.c"
        "${MC_AGENT_MINHOOK_SOURCE_DIR}/src/trampoline.c"
        "${MC_AGENT_MINHOOK_SOURCE_DIR}/src/hde/hde64.c")
    target_include_directories(mc_agent_minhook
        PUBLIC "${MC_AGENT_MINHOOK_SOURCE_DIR}/include"
        PRIVATE
            "${MC_AGENT_MINHOOK_SOURCE_DIR}/src"
            "${MC_AGENT_MINHOOK_SOURCE_DIR}/src/hde")
    set_target_properties(mc_agent_minhook PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        AUTOMOC OFF)
    set(MC_AGENT_MINHOOK_TARGET mc_agent_minhook)
endif()

if(NOT MC_AGENT_IMGUI_SOURCE_DIR OR
   NOT EXISTS "${MC_AGENT_IMGUI_SOURCE_DIR}/imgui.cpp" OR
   NOT EXISTS "${MC_AGENT_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp")
    message(FATAL_ERROR
        "Dear ImGui was not found. Set MC_AGENT_IMGUI_SOURCE_DIR, or explicitly "
        "configure with -DMC_AGENT_FETCH_DEPENDENCIES=ON.")
endif()

add_library(mc_agent_imgui STATIC
    "${MC_AGENT_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp")
add_library(mc_agent::imgui ALIAS mc_agent_imgui)
target_include_directories(mc_agent_imgui PUBLIC
    "${MC_AGENT_IMGUI_SOURCE_DIR}"
    "${MC_AGENT_IMGUI_SOURCE_DIR}/backends")
target_compile_definitions(mc_agent_imgui PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(mc_agent_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(McOverlayAgent SHARED
    include/mcoverlay/ControlProtocol.h
    src/AgentLog.cpp
    src/AgentLog.h
    src/AgentOptions.cpp
    src/AgentOptions.h
    src/AgentRuntime.cpp
    src/AgentRuntime.h
    bindings/BedWarsState.cpp
    bindings/BedWarsState.h
    bindings/GameBindings.cpp
    bindings/GameBindings.h
    bindings/MappingProvider.cpp
    bindings/MappingProvider.h
    ipc_client.cpp
    ipc_client.h
    jvm.cpp
    jvm.h
    opengl_hook.cpp
    opengl_hook.h
    overlay_renderer.cpp
    overlay_renderer.h
    wndproc_hook.cpp
    wndproc_hook.h
    McOverlayAgent.def)

target_include_directories(McOverlayAgent
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/bindings"
        "${MC_AGENT_JNI_INCLUDE_DIR}"
        "${MC_AGENT_JNI_PLATFORM_INCLUDE_DIR}"
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_definitions(McOverlayAgent PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(McOverlayAgent PRIVATE
    ${MC_AGENT_MINHOOK_TARGET}
    mc_agent::imgui
    advapi32
    dwmapi
    gdi32
    opengl32
    user32)

if(MSVC)
    target_compile_options(McOverlayAgent PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(mc_agent_imgui PRIVATE /W3 /EHsc)
else()
    target_compile_options(McOverlayAgent PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MINGW AND MC_AGENT_STATIC_MINGW_RUNTIME)
        target_link_options(McOverlayAgent PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

set_target_properties(McOverlayAgent PROPERTIES
    PREFIX ""
    OUTPUT_NAME "McOverlayAgent")

# Opt-in, Qt/JVM-free validation for the registry, strict dictionary schema,
# environment selection, Lunar legacy namespace, and freeze boundary.
add_executable(McOverlayMappingProviderTests EXCLUDE_FROM_ALL
    bindings/MappingProvider.cpp
    bindings/MappingProvider.h
    bindings/MappingProviderTests.cpp)
target_compile_features(McOverlayMappingProviderTests PRIVATE cxx_std_20)
target_include_directories(McOverlayMappingProviderTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings")
target_compile_definitions(McOverlayMappingProviderTests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(McOverlayMappingProviderTests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    target_compile_options(McOverlayMappingProviderTests PRIVATE
        /W4 /permissive- /EHsc)
else()
    target_compile_options(McOverlayMappingProviderTests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MINGW AND MC_AGENT_STATIC_MINGW_RUNTIME)
        target_link_options(McOverlayMappingProviderTests PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()
endif()

# Qt/JVM-free tests for the language-independent Sidebar parser and the
# canonical team/wool mappings used by bed ownership.
add_executable(McOverlayBedWarsStateTests EXCLUDE_FROM_ALL
    bindings/BedWarsState.cpp
    bindings/BedWarsState.h
    bindings/BedWarsStateTests.cpp)
target_compile_features(McOverlayBedWarsStateTests PRIVATE cxx_std_20)
target_include_directories(McOverlayBedWarsStateTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings")
set_target_properties(McOverlayBedWarsStateTests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    target_compile_options(McOverlayBedWarsStateTests PRIVATE
        /W4 /permissive- /EHsc)
else()
    target_compile_options(McOverlayBedWarsStateTests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MINGW AND MC_AGENT_STATIC_MINGW_RUNTIME)
        target_link_options(McOverlayBedWarsStateTests PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MC_AGENT_MINHOOK_LICENSE_FILE
        "${MC_AGENT_MINHOOK_SOURCE_DIR}/LICENSE.txt" PARENT_SCOPE)
    set(MC_AGENT_IMGUI_LICENSE_FILE
        "${MC_AGENT_IMGUI_SOURCE_DIR}/LICENSE.txt" PARENT_SCOPE)
endif()
