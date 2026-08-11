zephyr_library()
zephyr_library_sources(pinmux.c)
# Unconditional: cornix.dtsi declares the csoff behavior for every cornix
# board, and soft off must keep working even without the indicator shield
# (the LED flash degrades to a weak no-op there).
zephyr_library_sources(../../shields/cornix_indicator/behavior_cornix_soft_off.c)
zephyr_library_sources_ifdef(
  CONFIG_SHIELD_CORNIX_INDICATOR
  ../../shields/cornix_indicator/cornix_indicator.c
  ../../shields/cornix_indicator/cornix_recovery.c
)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
