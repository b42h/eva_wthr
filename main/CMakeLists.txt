idf_component_register(
    SRCS main.c eva_cdc.c eva_settings.c eva_clock.c eva_wifi_status.c
         eva_wifi.c eva_weather.c eva_weather_canvas.c eva_cloud_assets.c
         eva_ota.c eva_ota_state.c eva_ota_status.c
         eva_font_uk_22.c eva_font_uk_14.c eva_font_clock_144.c
         eva_font_clock_144_extralight.c eva_font_clock_144_extrabold.c
         eva_font_clock_288_extralight.c
         eva_screenshot.c
         weather_fetch.c weather_fetch_clearoutside.c weather_fetch_openmeteo.c
    INCLUDE_DIRS .
    REQUIRES espressif__esp_tinyusb
    PRIV_REQUIRES nvs_flash esp_timer esp_system espressif__esp32_p4_function_ev_board
                  esp_wifi esp_netif esp_event lwip esp_http_client json
                  esp_driver_ppa esp_driver_jpeg esp_lcd mbedtls esp_partition lz4
                  esp_http_server app_update espressif__mdns)

# EVA_PORTRAIT_NATIVE (phase A, 2026-07-04) REVERTED: portrait-native render
# removed the 15 ms PPA rotation but made every full-screen pixel write
# scattered (portrait stride 480 via eva_sbuf_idx), inflating cloud blend
# cl 30→100 ms — measured identical on CPU and PPA paths, so the cost is the
# scattered write, not the blend method. Net regression (8-10 Hz vs 15).
# The PPA rotation was cheaper than cache-hostile writes. Portrait code stays
# behind #ifdef but is not compiled. To re-enable, re-add the flag below.
# target_compile_options(${COMPONENT_LIB} PRIVATE -DEVA_PORTRAIT_NATIVE)

esptool_py_flash_to_partition(flash "storage" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/clouds.bin")
