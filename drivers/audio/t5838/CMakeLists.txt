# SPDX-License-Identifier: Apache-2.0
zephyr_include_directories(.)
zephyr_library()
zephyr_library_sources(t5838_alif_pdm.c)
zephyr_library_sources_ifdef(CONFIG_T5838_AAD_TRIGGER t5838_aad_mode.c)