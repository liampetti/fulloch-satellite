# Contributing to Fulloch Satellite

Thank you for contributing to Fulloch Satellite.

## Development Setup

1. Fork the repository and create a branch from `main`.
2. Install and activate ESP-IDF 5.3.1 using Espressif's installation guide.
3. Copy the example provisioning files to private files and replace their
   placeholder values:

```sh
cp provisioning/device.config.example.json provisioning/device.config.json
cp provisioning/device.credentials.example.json provisioning/device.credentials.json
```

## Validation

Run the host checks before opening a pull request:

```sh
python3 tools/check_format.py
python3 -m unittest discover -s tests -p 'test_*.py'
python3 tools/provision.py validate \
  --config provisioning/device.config.example.json \
  --credentials provisioning/device.credentials.example.json
```

Build the Touch LCD Rev 2 profile with the example bundle:

```sh
idf.py -B build/review \
  -D FULLOCH_BOARD=waveshare_touch_lcd_185c_box_rev2 \
  -D FULLOCH_PROVISIONING_CONFIG="$PWD/provisioning/device.config.example.json" \
  -D FULLOCH_PROVISIONING_CREDENTIALS="$PWD/provisioning/device.credentials.example.json" \
  build
```

## Guidelines

- Do not commit credentials, generated NVS images, certificates, private keys,
  or local build artifacts.
- Keep changes focused and update tests and documentation with behavior changes.
- Preserve the PSRAM allocation policy in `sdkconfig.defaults` when changing
  audio, display, or network code.
- Use clear commit messages and describe testing in pull requests.
