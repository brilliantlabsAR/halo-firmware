zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)

zephyr_include_directories(.)


zephyr_library_sources_ifdef(CONFIG_VIDEO_PAG7982	pag7982.c)

