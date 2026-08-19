#----------------------------------------------------------------------------
#  SPDX-FileCopyrightText: Copyright 2022, 2024 Arm Limited and/or its
#  affiliates <open-source-office@arm.com>
#  SPDX-License-Identifier: Apache-2.0
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#----------------------------------------------------------------------------

#######################################################
# Math functions for ML pipelines.                    #
#######################################################

project(arm_math
        DESCRIPTION "Collection of the optimised math functions for ML pipelines"
        LANGUAGES C CXX)

add_library(arm_math STATIC)

target_sources(arm_math
        PRIVATE
        PlatformMath.cc)

target_include_directories(arm_math PUBLIC include)
target_link_libraries(arm_math PRIVATE zephyr_interface)

message(STATUS "*******************************************************")
message(STATUS "Library                    : " arm_math)
message(STATUS "CMAKE_SYSTEM_PROCESSOR     : " ${CMAKE_SYSTEM_PROCESSOR})
message(STATUS "*******************************************************")
