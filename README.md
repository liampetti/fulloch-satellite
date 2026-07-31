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
- Set the Wi-Fi credentials and a unique `fulloch.device_token`.
- Set `server.url` to the local-network `wss://` endpoint exposed by the
  Fulloch server.
- Set `audio.speaker_volume_percent` from 0 to 100. This controls the ES8311
  codec output level.
- Leave `audio.aec_enabled` enabled for normal full-duplex operation. Disable
  it only for connection or audio-capture diagnostics.
- To use Spotify controls, set `spotify.enabled` and add the Spotify client
  credentials. Controls apply to Spotify's currently active device. The refresh
  token needs `user-modify-playback-state` and `user-read-playback-state`
  scopes, and the account requires Spotify Premium.

The firmware does not verify the server certificate for its local-network
WebSocket connection. Traffic remains encrypted, but this connection is
susceptible to a network man-in-the-middle attack. The Spotify client separately
uses the public CA bundle to verify Spotify HTTPS connections.

The private bundle files and generated NVS image are ignored by Git. They are a
development provisioning method, not encrypted production storage. Device
bundles do not accept TLS certificates or private keys.

### 3. Build And Flash

Connect the board with a data-capable USB cable. Identify the serial port, then
build and flash with the relevant port substituted for `<port>`:

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
board initializes. In the serial monitor, verify that Wi-Fi connects, the
WebSocket transport connects, and the server accepts the satellite handshake.
The device then streams microphone PCM uplink and plays accepted server PCM
downlink. Tap the screen while idle to see date and time and a small light symbol indicating connection state. Swipe either direction to change screen to Spotify music controls.

During a server-driven voice turn, the display shows layered pulsing circles
from wake detection through follow-up listening. A tap on the pulsing rings during an active voice turn sends a stop message,
clears queued audio, and returns the device to idle.

## Network Recovery

The satellite connects as a Wi-Fi station and uses the provisioned `wss://`
server URL. Wi-Fi and server transport retries use a bounded one- to 60-second
exponential backoff. Connection logs deliberately omit Wi-Fi credentials,
device tokens, and server URLs.

The clock uses SNTP after Wi-Fi connects and remains available if the server is
unavailable. Spotify controls connect directly to Spotify over HTTPS and do not
depend on the satellite WebSocket connection.

## Releases

Version tags publish the bootloader, partition table, OTA data, application
image, and checksums as GitHub release assets. Release assets do not include
`nvs.bin`; provision and flash each device's NVS partition locally before first
use.
