idf_component_register(
    SRCS "waveshare_touch_lcd_185c.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_timer espressif__esp_codec_dev
)
