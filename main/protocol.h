#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_websocket_client.h"

typedef enum {
    SATELLITE_ASSISTANT_IDLE,
    SATELLITE_ASSISTANT_WAKE_DETECTED,
    SATELLITE_ASSISTANT_LISTENING,
    SATELLITE_ASSISTANT_THINKING,
    SATELLITE_ASSISTANT_SPEAKING,
    SATELLITE_ASSISTANT_FOLLOW_UP,
    SATELLITE_ASSISTANT_ERROR,
} satellite_assistant_state_t;

esp_err_t satellite_protocol_init(void);
void satellite_protocol_reset(void);
bool satellite_protocol_connected(esp_websocket_client_handle_t websocket);
void satellite_protocol_handle_data(esp_websocket_client_handle_t websocket,
                                    const esp_websocket_event_data_t *event);
esp_err_t satellite_protocol_send_uplink(const int16_t *samples, size_t sample_count);
satellite_assistant_state_t satellite_protocol_assistant_state(void);
bool satellite_protocol_ready(void);
esp_err_t satellite_protocol_send_stop(void);
