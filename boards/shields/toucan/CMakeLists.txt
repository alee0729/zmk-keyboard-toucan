# Battery relay central — compile for the dongle (central) build only.
# The peripheral side lives in boards/shields/nice_view_gem/widgets/battery_relay_peripheral.c.
if(CONFIG_SHIELD_TOUCAN_DONGLE AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL
   AND NOT CONFIG_DONGLE_SCREEN_BATTERY_RELAY)
  zephyr_library_sources(battery_relay_central.c)
endif()
