# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Handle RELEASE build option
# Default is debug build
# Use environment variable: RELEASE=1 west build ... for release build
set(RELEASE OFF CACHE BOOL "Enable release build")

# Check environment variable first (works with sysbuild)
if(DEFINED ENV{RELEASE} AND "$ENV{RELEASE}" STREQUAL "1")
    set(RELEASE ON)
endif()

if(RELEASE)
    message(STATUS "=== Release build enabled ===")
    set(EXTRA_CONF_FILE "release.conf" CACHE STRING "Extra config file" FORCE)
    set(BUILD_EXTRAVERSION "")
else()
    message(STATUS "=== Debug build enabled (default) ===")
    set(EXTRA_CONF_FILE "debug.conf" CACHE STRING "Extra config file" FORCE)
    set(BUILD_EXTRAVERSION "debug")
endif()

# DIAG=1: the bring-up experiment build (diag.conf: /lfs/diag.txt event log and
# the staged boot). Tagged in the version string so it can't pass for a release.
if(DEFINED ENV{DIAG} AND "$ENV{DIAG}" STREQUAL "1")
    message(STATUS "=== Diagnostic (staged boot) build ===")
    set(EXTRA_CONF_FILE "${EXTRA_CONF_FILE};diag.conf" CACHE STRING "Extra config file" FORCE)
    if(BUILD_EXTRAVERSION STREQUAL "")
        set(BUILD_EXTRAVERSION "diag")
    else()
        set(BUILD_EXTRAVERSION "${BUILD_EXTRAVERSION}diag")
    endif()
endif()

# Update VERSION file with correct EXTRAVERSION
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VERSION_CONTENT)
string(REGEX REPLACE "EXTRAVERSION = [^\n]*" "EXTRAVERSION = ${BUILD_EXTRAVERSION}" VERSION_CONTENT "${VERSION_CONTENT}")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" "${VERSION_CONTENT}")

if(SYSBUILD)
    list(APPEND EXTRA_CONF_FILE "prj_sysbuild.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(halo)

include_directories("include")

file(GLOB_RECURSE SRC_FILES "src/*.c")
target_sources(app PRIVATE ${SRC_FILES})

libmpix_init(app)
