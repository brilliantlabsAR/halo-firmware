# Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license
#

zephyr_sources_ifdef(CONFIG_IMG_ASSETS
	sample_photo_alpha8.c
	sample_photo_argb1555.c
	sample_photo_argb4444.c
	sample_photo_argb8888.c
	sample_photo_rgb565.c
	sample_photo_rgb888.c
	sample_photo_rgba4444.c
	sample_photo_rgba5551.c
	sample_photo_rgba8888.c
)
