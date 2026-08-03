#pragma once

#include "lvgl.h"

typedef enum {
    SATELLITE_MUSIC_REFRESH,
    SATELLITE_MUSIC_PREVIOUS,
    SATELLITE_MUSIC_TOGGLE_PLAYBACK,
    SATELLITE_MUSIC_NEXT,
} satellite_music_command_t;

void satellite_music_request(satellite_music_command_t command);
const lv_img_dsc_t *satellite_music_album_art(void);
