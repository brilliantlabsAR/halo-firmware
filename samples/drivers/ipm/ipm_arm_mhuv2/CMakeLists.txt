# Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.20.0)

if("${BOARD}" STREQUAL "alif_e3_dk_rtss_he" OR
   "${BOARD}" STREQUAL "alif_e7_dk_rtss_he")
        add_subdirectory(rtss_he)
elseif("${BOARD}" STREQUAL "alif_e7_dk_rtss_hp" OR
       "${BOARD}" STREQUAL "alif_e3_dk_rtss_hp")
        add_subdirectory(rtss_hp)
endif()

project(mhuv2)
