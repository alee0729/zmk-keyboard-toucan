# Patch zmk-dongle-screen's battery_relay_peripheral.c which uses the
# deprecated ZMK_EVENT_RAISE(new_zmk_*(...)) API removed in ZMK v0.3.
# Exclude the broken file and substitute the fixed version from this repo.
if(DEFINED ZEPHYR_DONGLE_SCREEN_MODULE_DIR)
  set_source_files_properties(
    "${ZEPHYR_DONGLE_SCREEN_MODULE_DIR}/boards/shields/dongle_screen/src/battery_relay_peripheral.c"
    PROPERTIES HEADER_FILE_ONLY TRUE
  )
  if(CONFIG_DONGLE_SCREEN_BATTERY_RELAY AND NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_include_directories(
      "${ZEPHYR_DONGLE_SCREEN_MODULE_DIR}/boards/shields/dongle_screen/src"
    )
    zephyr_library_sources(
      "${CMAKE_CURRENT_LIST_DIR}/src/battery_relay_peripheral.c"
    )
  endif()
endif()
