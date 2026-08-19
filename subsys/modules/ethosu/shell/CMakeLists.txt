# Copyright 2021-2022 Arm Limited and/or its affiliates <open-source-office@arm.com>
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ethos_shell)

add_subdirectory(
    ../../../../lib/ethosu_utils
    ${CMAKE_BINARY_DIR}/lib/ethosu_utils
)

target_sources(app PRIVATE
		src/ethosu_shell.cpp
)
