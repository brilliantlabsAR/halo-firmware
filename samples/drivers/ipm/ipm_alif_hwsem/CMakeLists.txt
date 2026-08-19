# Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.20.0)

if(NOT("${BOARD}" STREQUAL "alif_e3_dk_rtss_he" OR
	"${BOARD}" STREQUAL "alif_e3_dk_rtss_hp" OR
	"${BOARD}" STREQUAL "alif_e7_dk_rtss_he" OR
	"${BOARD}" STREQUAL "alif_e7_dk_rtss_hp" OR
	"${BOARD}" STREQUAL "alif_e7_dk_apss"))
	message(FATAL_ERROR "${BOARD} was not supported for this sample")
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hwsem)

option(HWSEM_ALL "Build test case to verify all HWSEMs" OFF)
option(HWSEM0 "Build test case to verify HWSEM0" ON)
if(HWSEM_ALL)
	target_sources(app PRIVATE src/hwsem_test_all.c)
elseif(HWSEM0)
	target_sources(app PRIVATE src/hwsem0_test.c)
endif()
