cmake_minimum_required(VERSION 3.24)

# This directory can be configured on its own for a quick loader-only build,
# while remaining a normal add_subdirectory() of the Qt controller project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(McOverlayNativeLoader LANGUAGES CXX)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "McOverlayNativeLoader is a Windows-only target.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "McOverlayNativeLoader must be built as x64 to match the target JVM.")
endif()

add_executable(McOverlayNativeLoader main.cpp)
target_compile_features(McOverlayNativeLoader PRIVATE cxx_std_20)
target_compile_definitions(McOverlayNativeLoader PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    UNICODE
    _UNICODE)

if(MSVC)
    target_compile_options(McOverlayNativeLoader PRIVATE
        /W4 /permissive- /EHsc)
else()
    target_compile_options(McOverlayNativeLoader PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(MINGW)
    # Keep the deployment self-contained: this small helper must not require
    # libgcc, libstdc++, or libwinpthread DLLs next to the Qt application.
    target_link_options(McOverlayNativeLoader PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++)
endif()

set_target_properties(McOverlayNativeLoader PROPERTIES
    OUTPUT_NAME "McOverlayNativeLoader"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF)
