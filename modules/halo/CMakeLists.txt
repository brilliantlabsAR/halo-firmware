# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_HALO)

zephyr_library()
zephyr_include_directories(include)

# Add LittleFS include directory for file manager
if(CONFIG_HALO_FILE_MANAGER)
zephyr_include_directories(${ZEPHYR_LITTLEFS_MODULE_DIR})
endif()

# Memory Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_MEM_MANAGER src/mem_manager.c)

# File Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_FILE_MANAGER src/file_manager.c)

# Power Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_PM_MANAGER src/pm_manager.c)

# Watchdog Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_WATCHDOG_MANAGER src/watchdog_manager.c)

# LED Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_LED_MANAGER src/led_manager.c)

# Battery Manager Component
zephyr_library_sources_ifdef(CONFIG_HALO_BATTERY_MANAGER src/battery_manager.c)

zephyr_library_sources_ifdef(CONFIG_HALO_AUDIO_STREAM src/audio_stream.c)
zephyr_library_sources_ifdef(CONFIG_HALO_AUDIO_HPF src/audio_eq.c)
zephyr_library_sources_ifdef(CONFIG_HALO_AUDIO_AEC src/audio_aec.c)
if(CONFIG_HALO_AUDIO_AEC)
    # the NLMS inner loops need real optimization (Zephyr defaults to -Os);
    # -ffast-math lets GCC vectorize the float reductions with Helium/MVE
    set_source_files_properties(src/audio_aec.c PROPERTIES
        COMPILE_OPTIONS "-O3;-ffast-math")
    # AEC algorithm tuning (margin servo, envelope gate + sustained ceiling,
    # onset duck, ...) lives in the AEC_MARGIN_* / AEC_SUP_* #define defaults in
    # src/audio_aec.c, each with its worn-validation rationale inline. We ship a
    # single board, so the device-validated values ARE the source defaults - no
    # per-source COMPILE_DEFINITIONS override here. The old override diverged from
    # the .c defaults and masked the real config (e.g. the offline host suite in
    # tests/aec/host still compiled the pre-tuning values). Override per build for
    # A/B with -DAEC_...=<val>. Full chapters: tests/aec/README.md.
endif()
zephyr_library_sources_ifdef(CONFIG_HALO_SFXR src/sfxr.c)

# BLE Manager Component
if(CONFIG_HALO_BLE_MANAGER)
    zephyr_library_sources(
        src/ble_manager.c
        src/ble_connection.c
        src/ble_security.c
        src/ble_service.c
    )
    
    # BLE Battery Service (optional)
    zephyr_library_sources_ifdef(CONFIG_HALO_BLE_BATTERY_SERVICE src/ble_battery.c)
    
    # BLE Lua Service (optional)
    zephyr_library_sources_ifdef(CONFIG_HALO_BLE_LUA_SERVICE src/ble_lua.c)
    
    # BLE OTA Service (optional)
    zephyr_library_sources_ifdef(CONFIG_HALO_BLE_OTA_SERVICE src/ble_ota.c)

    # ANCS client (optional) - iOS notification access
    zephyr_library_sources_ifdef(CONFIG_HALO_BLE_ANCS_CLIENT src/ble_ancs.c)

    # BLE AUDIO Service (optional)
    if(CONFIG_HALO_BLE_AUDIO_SERVICE)
        zephyr_library_sources(
            src/ble_audio.c
            src/sdu_queue.c
            src/iso_datapath_ctoh.c
            src/iso_datapath_htoc.c
        )
    endif()
    
    # SE Management (requires BLE OTA Service)
    zephyr_library_sources_ifdef(CONFIG_HALO_SE_MGMT src/se_mgmt.c)

endif()

# Lua Runtime Component (depends on BLE Lua Service)
if(CONFIG_HALO_LUA_RUNTIME)
    zephyr_library_sources(
        src/lua_service.c
        src/lua_runtime.c
        src/lua_version.c
        src/lua_system.c
        src/lua_time.c
        src/lua_file.c
        src/lua_bluetooth.c
        src/lua_compression.c
        src/lua_button.c
        src/lua_imu.c
    )

    # frame.log reads the FS log backend's files; without the backend
    # there is nothing to list
    zephyr_library_sources_ifdef(CONFIG_LOG_BACKEND_FS src/lua_log.c)

    # ANCS Lua bindings (optional, requires the ANCS client)
    zephyr_library_sources_ifdef(CONFIG_HALO_BLE_ANCS_CLIENT src/lua_ancs.c)

    # Display support (optional, requires CONFIG_DISPLAY)
    zephyr_library_sources_ifdef(CONFIG_DISPLAY src/lua_display.c)

    # Boot logo splash (optional)
    zephyr_library_sources_ifdef(CONFIG_HALO_BOOT_LOGO src/boot_logo_data.c)

    # Camera support (optional, requires CONFIG_VIDEO and libmpix)
    zephyr_library_sources_ifdef(CONFIG_VIDEO src/lua_camera.c)
    zephyr_library_sources_ifdef(CONFIG_MPIX ${ZEPHYR_MPIX_MODULE_DIR}/bindings/lua/lua_mpix.c)
    
    # Audio stream abstraction (requires CONFIG_AUDIO)
    if(CONFIG_HALO_AUDIO_STREAM)        
        # Speaker support (requires audio stream)
        zephyr_library_sources_ifdef(CONFIG_HALO_LUA_SPEAKER src/lua_speaker.c)

        # Generated sound effects support (requires SFXR)
        zephyr_library_sources_ifdef(CONFIG_HALO_LUA_SOUND src/lua_sound.c)
        
        # Microphone support (requires audio stream)
        zephyr_library_sources_ifdef(CONFIG_HALO_LUA_MICROPHONE src/lua_microphone.c)
    endif()
    
endif()

endif() # CONFIG_HALO
