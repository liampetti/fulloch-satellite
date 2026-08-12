#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "connectivity.h"
#include "esp_err.h"

esp_err_t satellite_ui_start(void);
void satellite_ui_post_connection_status(satellite_connection_status_t status);
void satellite_ui_post_assistant_state(int state);
void satellite_ui_post_playback_activity(void);
void satellite_ui_post_follow_up_expiry(uint32_t duration_ms);
void satellite_ui_post_music_state(const char *track, bool playing);
void satellite_ui_post_music_message(const char *message);
void satellite_ui_post_album_art(void);
