# Battery and layer relay centrals — compile for the dongle (central) build only.
# The peripheral sides live in boards/shields/nice_view_gem/widgets/.
if(CONFIG_SHIELD_TOUCAN_DONGLE AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL
   AND NOT CONFIG_DONGLE_SCREEN_BATTERY_RELAY)
  zephyr_library_sources(battery_relay_central.c)
  zephyr_library_sources(layer_relay_central.c)
endif()
