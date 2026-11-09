# Fulloch Satellite

<p align="center">
  <img src="assets/fulloch-sat.png" alt="Fulloch Satellite">
</p>

Fulloch Satellite is ESP-IDF firmware for ESP32-S3 voice devices. It
captures microphone audio, streams it to the
[Fulloch server](https://github.com/liampetti/fulloch), plays back audio, and shows Fulloch activity on the device screen.

## Hardware Support

- **[Waveshare ESP32-S3 Touch LCD 1.85C Box Rev 2](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C):** primary supported profile.

The Touch LCD firmware supports only boards marked **Rev 2.0**. Do not flash it
to a Rev 1 board.

## Setup And Flashing

### 1. Install ESP-IDF

Install and activate ESP-IDF 5.3.1 using the
[official ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32/get-started/).
Run the commands below from an environment where `idf.py` is available:

```sh
idf.py --version
```

### 2. Create A Device Bundle

Device configuration and credentials are supplied as separate JSON files and
compiled into the NVS partition. Copy the tracked examples to private files:

```sh
cp provisioning/device.config.example.json provisioning/device.config.json
cp provisioning/device.credentials.example.json provisioning/device.credentials.json
```

Replace every example value before flashing. In particular:

- Set unique `device.id` and `device.name` values.
- Set `device.timezone` to a supported IANA name or a POSIX TZ string, as
  described below.
- Set the Wi-Fi credentials and a unique `fulloch.device_token`.
- Set `server.url` to the local-network `wss://` endpoint exposed by the
  Fulloch server.
- Set `server.certificate_pem` to the public PEM trust certificate for that
  server (the self-signed dashboard certificate, or the CA certificate that
  issued it). Encode its newlines as `\n` in JSON. Never include a private key.
  The certificate is limited to 3,999 UTF-8 bytes and is stored in the 64 KiB
  NVS partition. The URL hostname or IP must match the server certificate.
  The device waits for SNTP before connecting so certificate dates are checked.
  Reprovision after changing the server address or its trust certificate.
  Existing bundles without a certificate must be updated before rebuilding;
  older provisioned devices must be reflashed with a new bundle and firmware.
- Set `audio.speaker_volume_percent` from 0 to 100. This controls the ES8311
  codec output level.
- Leave `audio.aec_enabled` enabled to reduce speaker feedback.
- Leave `audio.uplink_channels` at `1` for the established mono AEC uplink.
  Set it to `2` only after the server supports negotiated dual-channel AEC
  processing; this requires `audio.aec_enabled`.
- Set `display.rotation_degrees` to `0` for the normal orientation or `180` to
  restore the previous upside-down display and touch orientation.
- Select the music source with `music.player`: `none` disables the music view and needs no music credentials; `spotify` enables Spotify controls and requires Spotify Premium and Spotify client credentials; `sonos` controls a Sonos speaker at `sonos.host`.

The example json is a valid Spotify configuration. To use Sonos, replace
its `music` and `spotify` sections with the following, and remove the
`spotify` section from the credentials file:

```json
"music": {
  "player": "sonos"
},
"sonos": {
  "host": "192.168.1.50"
}
```

### Device Timezone

The firmware supports these IANA timezone names:

- `Australia/Sydney`, `Australia/Melbourne`, `Australia/Brisbane`, `Australia/Perth`
- `America/Los_Angeles`, `America/Denver`, `America/Phoenix`, `America/Chicago`, `America/New_York`
- `Europe/London`, `Europe/Berlin`, `Europe/Paris`, `Etc/UTC`

For other regions, supply a POSIX TZ string instead of an IANA name, for
example `PST8PDT,M3.2.0,M11.1.0` for Pacific time or
`GMT0BST,M3.5.0/1,M10.5.0` for London. POSIX offsets have the opposite sign
to UTC offsets; include daylight-saving transition rules where needed. The
firmware has no full timezone database, and these rules may need updating when
local laws change. Unsupported IANA-style names cause a startup error in the
device log. Other strings are passed to the C library without validation, so
an invalid POSIX string may still produce the wrong time. Reprovision and flash
after changing the timezone.

### Wizard Bundle Contract

The USB wizard should construct schema version `1` for both JSON objects and
pass them through the same bundle validation as the CLI. The following fields
are specific to the Waveshare profile; no Home Assistant entity is needed.
Secrets are requested for each attempt and must not be saved as retry state.
String limits below are UTF-8 bytes, not Unicode character counts.

| Section | Field | Wizard default / constraint |
| --- | --- | --- |
| `device` | `id`, `name`, `timezone` | Required nonempty strings, at most 64 characters each. Choose a unique ID. Timezone must be a supported IANA name or POSIX TZ string (see Device Timezone). |
| `server` | `url`, `certificate_pem` | Required `wss://` URL (up to 256 characters) and one public PEM trust certificate (up to 3,999 bytes). The user must confirm the reachable address. |
| `wifi` credentials | `ssid`, `password` | Required nonempty strings, up to 32 and 63 characters respectively; do not prefill the password from prior attempts. |
| `fulloch` credentials | `device_token` | Generated uniquely by the server, not entered as a CLI argument or displayed. Up to 256 characters. |
| `audio` | `speaker_volume_percent`, `aec_enabled`, `uplink_channels` | Defaults: 65, true, 1. Volume is 0–100; uplink is 1 or 2, and 2 requires AEC. |
| `display` | `rotation_degrees` | Defaults to 0; accepts 0 or 180. |
| `music` | `player` | Default to `none`; alternatives are `spotify` and `sonos`. |
| `spotify` credentials | `client_id`, `client_secret`, `refresh_token` | Required only for Spotify; limits are 256, 256 and 512 characters. Never store in retry state. |
| `sonos` | `host` | Required only for Sonos, up to 63 characters. |

For `none`, omit both `spotify` credentials and the `sonos` configuration.
The example certificate is illustrative only; replace it with the actual public
trust certificate for the server.

The NVS generator is also available without an ESP-IDF installation. Install the
pinned `requirements-provision.txt` in a Python environment, then run
`python tools/provision.py stage --config <config.json> --credentials <credentials.json> --output <nvs.bin> --size 0x10000`.
The generated image contains secrets: restrict access and remove it after use.
This standalone generation does not build or flash firmware.
Fulloch vendors the identical `tools/provision.py` as
`server/satellite_bundle.py` and installs the same pinned generator in both
server images. Update both copies together; the host tests check for drift when
both repositories are checked out as siblings.

### Release Artifacts

Build with a specified `PROJECT_VER`, then export with
`python tools/firmware_manifest.py --build build/touch-lcd-rev2 --version <version> --output <new-output-directory>`.
The exporter checks the version embedded in the application and the compiled
partition table before copying the public images. Its manifest records SHA-256
checksums, byte sizes and flash offsets; the 64 KiB NVS slot is described but
no device-specific NVS image, credentials or private key are published.
The shipped `0.1.0` images were built with ESP-IDF 5.3.1. They have not yet
passed the plan's physical Rev 2 hardware acceptance tests.

For the optional certificate-verification host test, build the ESP-IDF 5.3.1
mbedTLS submodule for the host, compile `tests/verify_certificate.c` against
its X.509 and crypto libraries, and set `SATELLITE_MBEDTLS_VERIFY` to the
resulting executable when running the host tests. This checks trusted,
untrusted, and hostname-mismatched certificates using the ESP-IDF mbedTLS
implementation; it does not replace testing the actual board.

### 3. Build And Flash

Connect the board with a data-capable USB cable. Identify the serial port, then build and flash with the relevant port substituted for `<port>`:

```sh
idf.py -B build/touch-lcd-rev2 \
  -D FULLOCH_BOARD=waveshare_touch_lcd_185c_box_rev2 \
  -D FULLOCH_PROVISIONING_CONFIG="$PWD/provisioning/device.config.json" \
  -D FULLOCH_PROVISIONING_CREDENTIALS="$PWD/provisioning/device.credentials.json" \
  -p <port> build flash
```

Typical ports are `/dev/cu.usbmodem*` or `/dev/cu.usbserial-*` on macOS and
`/dev/ttyACM*` or `/dev/ttyUSB*` on Linux. If the board is not detected, hold
its BOOT control while starting the command and release it when flashing begins.

To monitor startup logs:

```sh
idf.py -B build/touch-lcd-rev2 -p <port> monitor
```

Exit the monitor with `Ctrl+]`.

### 4. Confirm Startup

The display progresses through audio and network status messages while the
board initializes. Tap the screen while idle to see date and time and a small led icon indicating connection state. Swipe either direction to change screen to music controls for the selected player.

During a server-driven voice turn, the display shows layered pulsing circles
from wake detection through follow-up listening. A short tap on the pulsing
rings during an active voice turn sends a stop message, clears queued audio,
and returns the device to idle.

Hold the display for 1.5 seconds to toggle Conversation mode. The server
confirms the change before the display updates: Conversation mode uses a
continuous blue-green ring pulse to show that wakeword-free listening is active;
normal mode returns to the usual green turn-only pulse. A long press also stops
any in-flight turn before changing modes. Swipes and music controls retain their
existing behavior.
