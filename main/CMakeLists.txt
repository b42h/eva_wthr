idf_component_register(
    SRCS main.c eva_cdc.c eva_settings.c eva_clock.c eva_wifi_status.c eva_fps_overlay.c
         eva_wifi.c eva_weather.c eva_weather_canvas.c
         eva_font_uk_22.c eva_font_uk_14.c eva_font_clock_144.c
         eva_font_clock_144_extralight.c eva_font_clock_144_extrabold.c
         eva_screenshot.c
         weather_fetch.c weather_fetch_clearoutside.c weather_fetch_openmeteo.c
    INCLUDE_DIRS .
    REQUIRES espressif__esp_tinyusb
    PRIV_REQUIRES nvs_flash esp_timer esp_system espressif__esp32_p4_function_ev_board
                  esp_wifi esp_netif esp_event lwip esp_http_client json
                  esp_driver_ppa esp_driver_jpeg esp_lcd mbedtls)
