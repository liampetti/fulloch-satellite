#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    SATELLITE_CONNECTION_STARTING,
    SATELLITE_CONNECTION_WIFI_CONNECTING,
    SATELLITE_CONNECTION_WIFI_READY,
    SATELLITE_CONNECTION_SERVER_CONNECTING,
    SATELLITE_CONNECTION_RECOVERING,
    SATELLITE_CONNECTION_CONNECTED,
    SATELLITE_CONNECTION_ERROR,
    SATELLITE_CONNECTION_UNAVAILABLE,
} satellite_connection_status_t;

esp_err_t satellite_connectivity_start(void);
bool satellite_connectivity_time_synchronized(void);
void satellite_connectivity_reconnect(void);
