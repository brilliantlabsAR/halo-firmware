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
	src/aipl/
	src/graphics/
)

# Add app sources
target_sources(app PRIVATE
	src/aipl/cpu_cache.c
	src/aipl/video_alloc.c
	src/graphics/image.c
	src/main.c
)

if (NOT CONFIG_AIPL_DAVE2D_ACCELERATION)
	# Add D/AVE2D functions directly to be able to use it
	target_sources(app PRIVATE ../../../lib/aipl/source/aipl_dave2d.c)
endif()
