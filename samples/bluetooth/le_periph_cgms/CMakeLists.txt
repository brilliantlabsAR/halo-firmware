# Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(le_periph_cgms)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/CMakeLists.txt)

# Include directories
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/inc
	${COMMON_INC}
)

target_sources(app PRIVATE
	src/main.c
	${COMMON_SRCS}
)

target_sources_ifdef(CONFIG_ALIF_BLE_ROM_IMAGE_V1_0	app PRIVATE src/cgms_rom_1_0.c)
target_sources_ifdef(CONFIG_ALIF_BLE_ROM_IMAGE_V1_2	app PRIVATE src/cgms_rom_1_2.c)
