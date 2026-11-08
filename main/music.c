#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "music.h"
#include "sonos.h"
#include "spotify.h"
#include "ui.h"

typedef enum {
    MUSIC_PROVIDER_SPOTIFY,
    MUSIC_PROVIDER_SONOS,
    MUSIC_PROVIDER_NONE,
} music_provider_t;

static const char *TAG = "music";
static music_provider_t provider = MUSIC_PROVIDER_SPOTIFY;
static bool provider_loaded;

static void load_provider(void)
{
    if (provider_loaded) {
        return;
    }
    provider_loaded = true;
    nvs_handle_t handle;
    if (nvs_open("music", NVS_READONLY, &handle) != ESP_OK) {
        return; // Existing Spotify provisioning has no music namespace.
    }
    char player[8];
    size_t length = sizeof(player);
    const esp_err_t error = nvs_get_str(handle, "player", player, &length);
    nvs_close(handle);
    if (error == ESP_OK && strcmp(player, "sonos") == 0) {
        provider = MUSIC_PROVIDER_SONOS;
    } else if (error == ESP_OK && strcmp(player, "none") == 0) {
        provider = MUSIC_PROVIDER_NONE;
    } else if (error == ESP_OK && strcmp(player, "spotify") != 0) {
        ESP_LOGW(TAG, "Unknown provisioned music player");
    }
}

void satellite_music_request(satellite_music_command_t command)
{
    load_provider();
    if (provider == MUSIC_PROVIDER_SONOS) {
        satellite_sonos_request((satellite_sonos_command_t)command);
    } else if (provider == MUSIC_PROVIDER_SPOTIFY) {
        satellite_spotify_request((satellite_spotify_command_t)command);
    }
}

const lv_img_dsc_t *satellite_music_album_art(void)
{
    load_provider();
    if (provider == MUSIC_PROVIDER_SPOTIFY) {
        return satellite_spotify_album_art();
    }
    return provider == MUSIC_PROVIDER_SONOS ? satellite_sonos_album_art() : NULL;
}

bool satellite_music_enabled(void)
{
    load_provider();
    return provider != MUSIC_PROVIDER_NONE;
}
