idf_component_register(
    SRCS "app_main.c" "audio.c" "connectivity.c" "fulloch_sat_logo.c" "music.c" "protocol.c" "sonos.c" "spotify.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_app_format esp_event esp_http_client esp_jpeg esp_netif esp_timer esp-tls esp_wifi esp_websocket_client espressif__esp-sr json lvgl mbedtls nvs_flash waveshare_touch_lcd_185c
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FULLOCH_BOARD="${FULLOCH_BOARD}"
)
