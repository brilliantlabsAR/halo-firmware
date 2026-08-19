# Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(alif_kws)

# Silence "changed in GCC 7.1" warnigns from C++ headers
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-psabi")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-psabi")

set(PLATFORM_DRIVERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/source/platform/zephyr)

add_subdirectory(src/math)
add_subdirectory(src/application/api/common)
add_subdirectory(src/application/api/use_case/kws)

target_include_directories(app PRIVATE
    src/application/main/include
    src/use_case/alif_kws/include
    src/application/api/use_case/kws/include
    src/generated/kws
)

target_compile_definitions(app PRIVATE ACTIVATION_BUF_SZ=CONFIG_ACTIVATION_BUF_SZ)

if (CONFIG_USE_APP_MENU)
target_compile_definitions(app PRIVATE USE_APP_MENU)
endif()

target_sources(app PRIVATE
    src/application/main/Main.cc
    src/use_case/alif_kws/src/MainLoop.cc
    src/use_case/alif_kws/src/UseCaseHandler.cc
    src/use_case/alif_kws/src/AudioBackend.cpp
    src/generated/kws/Labels.cc
)

if (CONFIG_ARM_ETHOS_U55_256)
target_sources(app PRIVATE src/generated/kws/kws_micronet_m_vela_H256.tflite.cc)
else()
target_sources(app PRIVATE src/generated/kws/kws_micronet_m_vela_H128.tflite.cc)
endif()

target_link_libraries(app PRIVATE
    arm_math
    common_api
    kws_api
)
