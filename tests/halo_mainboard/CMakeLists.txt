# Copyright (c) 2026 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(halo_mainboard)

# The FT BEGIN line names the firmware release this image ships with and the
# commit it was built from, so a station log ties each result to exact bytes.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../../applications/halo/VERSION version_lines)
foreach(line ${version_lines})
    if(line MATCHES "^(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL) *= *([0-9]+)")
        set(${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endif()
endforeach()
execute_process(
    COMMAND git describe --abbrev=12 --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE mbt_git
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
target_compile_definitions(app PRIVATE
    MBT_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${PATCHLEVEL}"
    MBT_GIT="${mbt_git}")

target_sources(app PRIVATE src/main.c src/ble_adv.c)
