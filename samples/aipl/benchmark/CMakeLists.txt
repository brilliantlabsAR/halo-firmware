# Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aipl_example)

# Add include directories
zephyr_include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/src/
	${CMAKE_CURRENT_SOURCE_DIR}/src/aipl/
	${CMAKE_CURRENT_SOURCE_DIR}/src/cpu_usage/
	${CMAKE_CURRENT_SOURCE_DIR}/src/fps_counter/
	${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/
	${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/objects/
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/
	${CMAKE_CURRENT_SOURCE_DIR}/src/utimer/)

# Add app sources
target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src/aipl/cpu_cache.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/aipl/video_alloc.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/cpu_usage/cpu_usage.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/fps_counter/fps_counter.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/objects/image.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/objects/rectangle.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/graphics.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/color_conversion_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/color_correction_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/cropping_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/draw_object_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/flipping_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/lut_transform_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/resize_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/rotation_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/perf_tests/white_balance_test.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utimer/utmr.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)

if(NOT CONFIG_AIPL_DAVE2D_ACCELERATION)
	# Add D/AVE2D functions directly to be able to use it
	target_sources(app PRIVATE ../../../lib/aipl/source/aipl_dave2d.c)
endif()
