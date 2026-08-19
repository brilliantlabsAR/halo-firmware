# Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.13.1)

if(CONFIG_CHIP_FACTORY_DATA)
set(EXTRA_DTC_OVERLAY_FILE "b1_factorydata_partition.overlay")
set(OVERLAY_CONFIG "factory_data.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(matter-lighting-switch)

set(CHIP_ROOT ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR})
set(ALIF_COMMON ${ZEPHYR_SDK_ALIF_MODULE_DIR}/subsys/matter)
set(MATTER_GEN_DIR ${CHIP_ROOT}/zzz_generated/)

include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/alif/app/enable-gnu-std.cmake)
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/chip_data_model.cmake)

# -Wmaybe-uninitialized has too many false positives, including on std::optional
# and chip::Optional.  Make it nonfatal.
#
# See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80635
target_compile_options(app PRIVATE -Werror -Wno-error=maybe-uninitialized)

target_include_directories(app PRIVATE
                           src/include
                           ${MATTER_GEN_DIR}
                           ${MATTER_GEN_DIR}/app-common
                           ${MATTER_GEN_DIR}/light-switch-app
                           ${ALIF_COMMON}/common
                           ${ALIF_COMMON}/icd
                           ${ALIF_COMMON}/binding)

target_sources(app PRIVATE
               src/main.cpp
               src/AppTask.cpp
               src/LightSwitch.cpp
               src/ShellCommands.cpp
               src/ZclCallbacks.cpp
               ${ALIF_COMMON}/icd/icdHandler.cpp
               ${ALIF_COMMON}/binding/BindingHandler.cpp
               ${ALIF_COMMON}/common/MatterStack.cpp
               ${ALIF_COMMON}/common/MatterUi.cpp
               ${ALIF_COMMON}/common/FabricTableDelegate.cpp)
              

chip_configure_data_model(app
    INCLUDE_SERVER
    ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/light-switch-app.zap
)
