cmake_minimum_required(VERSION 3.24)

if(NOT WIN32)
    return()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "McOverlayOpenGlJvmSmoke requires a Windows x64 build.")
endif()

# Keep this target opt-in: normal application builds and packages should not
# acquire an extra test executable. Build it explicitly with
# `cmake --build <build-dir> --target McOverlayOpenGlJvmSmoke`.
add_executable(McOverlayOpenGlJvmSmoke EXCLUDE_FROM_ALL
    OpenGlJvmSmokeHarness.cpp)
set_target_properties(McOverlayOpenGlJvmSmoke PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF)
target_compile_features(McOverlayOpenGlJvmSmoke PRIVATE cxx_std_20)
target_compile_definitions(McOverlayOpenGlJvmSmoke PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(McOverlayOpenGlJvmSmoke PRIVATE
    "${MC_AGENT_JNI_INCLUDE_DIR}"
    "${MC_AGENT_JNI_PLATFORM_INCLUDE_DIR}")
target_link_libraries(McOverlayOpenGlJvmSmoke PRIVATE
    gdi32
    opengl32
    user32)
add_dependencies(McOverlayOpenGlJvmSmoke McOverlayAgent)

add_executable(McOverlayHotkeyCaptureTests EXCLUDE_FROM_ALL
    HotkeyCaptureServiceTests.cpp
    ../src/HotkeyCaptureService.cpp
    ../src/HotkeyCaptureService.h)
target_compile_features(McOverlayHotkeyCaptureTests PRIVATE cxx_std_20)
target_include_directories(McOverlayHotkeyCaptureTests PRIVATE ../src)
target_link_libraries(McOverlayHotkeyCaptureTests PRIVATE Qt6::Core Qt6::Gui)

if(MSVC)
    target_compile_options(McOverlayOpenGlJvmSmoke PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(McOverlayOpenGlJvmSmoke PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MINGW)
        target_link_options(McOverlayOpenGlJvmSmoke PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    endif()
endif()
