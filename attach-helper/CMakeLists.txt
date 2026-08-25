cmake_minimum_required(VERSION 3.21)

# This file works both as a standalone project and through add_subdirectory().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(McOverlayAttachHelper LANGUAGES NONE)
endif()

# Find only the two JDK build tools we actually need. FindJava's Development
# component can accidentally combine a registry-selected Java 8 JRE with a
# newer javac on Windows, then fail while looking for the removed `javah` tool.
# Keeping these cache variables explicit also lets a parent build select a
# bundled JDK deterministically.
find_program(MC_OVERLAY_JAVAC_EXECUTABLE
    NAMES javac
    HINTS "$ENV{JAVA_HOME}/bin"
    DOC "Java compiler used to build the Attach helper")
find_program(MC_OVERLAY_JAR_EXECUTABLE
    NAMES jar
    HINTS "$ENV{JAVA_HOME}/bin"
    DOC "JDK jar tool used to package the Attach helper")

if(NOT MC_OVERLAY_JAVAC_EXECUTABLE OR NOT MC_OVERLAY_JAR_EXECUTABLE)
    message(FATAL_ERROR
        "A full JDK 8 or newer is required (both javac and jar must be available). "
        "Set JAVA_HOME to the JDK installation directory if necessary.")
endif()

set(ATTACH_HELPER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/com/mcoverlay/attach/AttachHelper.java")
set(ATTACH_HELPER_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/MANIFEST.MF")
set(ATTACH_HELPER_CLASSES_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(ATTACH_HELPER_JAR
    "${CMAKE_CURRENT_BINARY_DIR}/McOverlayAttachHelper.jar")

add_custom_command(
    OUTPUT "${ATTACH_HELPER_JAR}"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${ATTACH_HELPER_CLASSES_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ATTACH_HELPER_CLASSES_DIR}"
    # -source/-target keep the bytecode and syntax Java 8 compatible while still
    # allowing this target to be built by current JDK releases.
    COMMAND "${MC_OVERLAY_JAVAC_EXECUTABLE}"
            -encoding UTF-8
            -source 8
            -target 8
            -Xlint:-options
            -d "${ATTACH_HELPER_CLASSES_DIR}"
            "${ATTACH_HELPER_SOURCE}"
    # Use the Java 8-compatible jar command syntax; do not use long options that
    # were introduced with the module-era JDK tools.
    COMMAND "${MC_OVERLAY_JAR_EXECUTABLE}"
            cfm "${ATTACH_HELPER_JAR}"
            "${ATTACH_HELPER_MANIFEST}"
            -C "${ATTACH_HELPER_CLASSES_DIR}" .
    DEPENDS
        "${ATTACH_HELPER_SOURCE}"
        "${ATTACH_HELPER_MANIFEST}"
    COMMENT "Building the Java 8-compatible JVM Attach helper"
    VERBATIM)

add_custom_target(mc_overlay_attach_helper ALL
    DEPENDS "${ATTACH_HELPER_JAR}")

# Export the artifact path when this directory is consumed with
# add_subdirectory(attach-helper). The parent project can copy/install the JAR
# without duplicating knowledge of this target's binary directory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(FILES "${ATTACH_HELPER_JAR}" DESTINATION .)
else()
    set(MC_OVERLAY_ATTACH_HELPER_JAR "${ATTACH_HELPER_JAR}" PARENT_SCOPE)
endif()
