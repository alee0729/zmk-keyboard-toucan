if(CONFIG_ZMK_DISPLAY AND CONFIG_NICE_VIEW_WIDGET_STATUS)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(assets/images.c)
  zephyr_library_sources(widgets/battery.c)
  zephyr_library_sources(widgets/battery_peripheral.c)
  zephyr_library_sources(widgets/layer.c)
  zephyr_library_sources(widgets/output.c)
  zephyr_library_sources(widgets/util.c)
  zephyr_library_sources(widgets/screen.c)
  zephyr_library_sources(widgets/sleep.c)

  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(widgets/profile.c)
  else()
    # Peripheral role: display relay receiver (battery + layer via single GATT service).
    # When CONFIG_DONGLE_SCREEN_BATTERY_RELAY is set, the zmk-dongle-screen module
    # compiles its own battery_relay_peripheral.c at module level — skip the local
    # copy to avoid a duplicate battery_relay_svc GATT service definition.
    if(NOT CONFIG_DONGLE_SCREEN_BATTERY_RELAY)
      zephyr_library_sources(widgets/battery_relay_peripheral.c)
    endif()
  endif()
endif()
