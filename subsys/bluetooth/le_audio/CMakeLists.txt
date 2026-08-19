# Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

zephyr_library()

zephyr_library_sources(
    audio_encoder.c
    audio_decoder.c
    audio_utils.c
    sdu_queue.c
    audio_queue.c
    audio_source_i2s.c
    audio_sink_i2s.c
    audio_i2s_common.c
    iso_datapath_htoc.c
    iso_datapath_ctoh.c
    presentation_compensation.c
)
