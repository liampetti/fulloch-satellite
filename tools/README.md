# Development Tools

`provision.py` validates private device bundles and uses ESP-IDF's NVS generator
to stage an image. Run it from an ESP-IDF activated shell:

```sh
python tools/provision.py validate \
  --config provisioning/device.config.json \
  --credentials provisioning/device.credentials.json
```
