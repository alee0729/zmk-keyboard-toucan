# Patch zmk-dongle-screen's battery_relay_peripheral.c which uses the
# deprecated ZMK_EVENT_RAISE(new_zmk_*(...)) API removed in ZMK v0.3.
# Exclude the broken file and substitute the fixed version from this repo.
if(DEFINED ZEPHYR_DONGLE_SCREEN_MODULE_DIR)
  set_source_files_properties(
    "${ZEPHYR_DONGLE_SCREEN_MODULE_DIR}/boards/shields/dongle_screen/src/battery_relay_peripheral.c"
    PROPERTIES HEADER_FILE_ONLY TRUE
  )
  # nice_view_gem has its own relay receiver widget for peripheral builds.
  # Do not compile the dongle_screen relay receiver patch alongside it, or
  # both services may register and consume writes with different event types.
  if(CONFIG_DONGLE_SCREEN_BATTERY_RELAY
     AND NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL
     AND NOT (CONFIG_ZMK_DISPLAY AND CONFIG_NICE_VIEW_WIDGET_STATUS))
    zephyr_library_include_directories(
      "${ZEPHYR_DONGLE_SCREEN_MODULE_DIR}/boards/shields/dongle_screen/src"
    )
    zephyr_library_sources(
      "${CMAKE_CURRENT_LIST_DIR}/src/battery_relay_peripheral.c"
    )
  endif()
endif()
