# Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

zephyr_include_directories(.)

add_subdirectory_ifdef(CONFIG_ALIF_BLE_HOST bluetooth)
add_subdirectory(powermgr)
add_subdirectory(modules)
add_subdirectory(dbuf_display)
add_subdirectory(img_assets)
