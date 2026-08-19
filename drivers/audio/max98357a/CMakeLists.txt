zephyr_library()

zephyr_library_sources_ifdef(CONFIG_MAX98357A_AUDIO max98357a_audio.c)
zephyr_library_sources_ifdef(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ speaker_protect.c)

zephyr_library_include_directories(include)
# expose the TX-tap API (AEC reference feed) to the halo module
zephyr_include_directories(include)
