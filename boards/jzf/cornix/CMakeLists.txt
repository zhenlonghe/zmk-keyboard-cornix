zephyr_library()
zephyr_library_sources(pinmux.c)
zephyr_library_sources_ifdef(
  CONFIG_SHIELD_CORNIX_INDICATOR
  ../../shields/cornix_indicator/cornix_indicator.c
)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
