zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)

zephyr_library_sources_ifdef(CONFIG_BMA580
	bma580/bma5.c
	bma580/bma580.c
	bma580/bma580_features.c
	bma580/bma580_driver.c
)
zephyr_library_sources_ifdef(CONFIG_QMC6308 qmc6308.c)
zephyr_library_sources_ifdef(CONFIG_ALIF_VBAT alif_vbat.c)