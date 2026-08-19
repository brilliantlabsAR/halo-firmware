# Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

set(SDK_ALIF_DIR ${CMAKE_CURRENT_LIST_DIR})

add_subdirectory(drivers)
add_subdirectory(subsys)
add_subdirectory(modules)

# Our drivers directory contains syscalls, so must be added to this list
list(APPEND SYSCALL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include/drivers)
set(SYSCALL_INCLUDE_DIRS ${SYSCALL_INCLUDE_DIRS} PARENT_SCOPE)
zephyr_include_directories(include)
