# Battery and layer relay central — compile for the dongle (central) build only.
# The peripheral side lives in boards/shields/nice_view_gem/widgets/.
# NOTE: CONFIG_DONGLE_SCREEN_BATTERY_RELAY=y is also set, but the dongle_screen
# module's central requires CONFIG_SHIELD_DONGLE_SCREEN which is not set for the
# toucan dongle. We must always compile our own central here.
if(CONFIG_SHIELD_TOUCAN_DONGLE AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(battery_relay_central.c)
endif()
