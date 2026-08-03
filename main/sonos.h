#pragma once

typedef enum {
    SATELLITE_SONOS_REFRESH,
    SATELLITE_SONOS_PREVIOUS,
    SATELLITE_SONOS_TOGGLE_PLAYBACK,
    SATELLITE_SONOS_NEXT,
} satellite_sonos_command_t;

void satellite_sonos_request(satellite_sonos_command_t command);
const lv_img_dsc_t *satellite_sonos_album_art(void);
#include "lvgl.h"
