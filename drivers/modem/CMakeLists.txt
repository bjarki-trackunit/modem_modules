zephyr_library()

zephyr_library_sources_ifdef(CONFIG_QUECTEL_BGXX quectel_bgxx.c)