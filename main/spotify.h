#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef enum {
    SATELLITE_SPOTIFY_REFRESH,
    SATELLITE_SPOTIFY_PREVIOUS,
    SATELLITE_SPOTIFY_TOGGLE_PLAYBACK,
    SATELLITE_SPOTIFY_NEXT,
} satellite_spotify_command_t;

esp_err_t satellite_spotify_start(void);
void satellite_spotify_request(satellite_spotify_command_t command);
const lv_img_dsc_t *satellite_spotify_album_art(void);
