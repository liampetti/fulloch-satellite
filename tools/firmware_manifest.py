#!/usr/bin/env python3
"""Export public Rev 2 images and a checked flash manifest from an ESP-IDF build."""

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def export(build: Path, destination: Path, version: str) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,30}", version):
        raise ValueError("invalid firmware version")
    flash = json.loads((build / "flasher_args.json").read_text())["flash_files"]
    images = {int(offset, 0): (build / path).resolve() for offset, path in flash.items()}
    for path in images.values():
        if not path.is_relative_to(build.resolve()):
            raise ValueError("flash image is outside the build directory")
    if not {0, 0x8000, 0x19000, 0x20000}.issubset(images):
        raise ValueError("build lacks required Rev 2 flash images")
    partition = images[0x8000].read_bytes()
    partitions = {}
    for position in range(0, len(partition) - 31, 32):
        magic, typ, subtype, offset, size, name, flags = struct.unpack_from("<HBBII16sI", partition, position)
        if magic != 0x50AA:
            break
        partitions[name.split(b"\0", 1)[0].decode("ascii")] = (typ, subtype, offset, size)
    if partitions.get("nvs") != (1, 2, 0x9000, 0x10000):
        raise ValueError("build NVS partition does not match the provisioner")
    if partitions.get("otadata") != (1, 0, 0x19000, 0x2000):
        raise ValueError("build OTA data partition does not match the Rev 2 profile")
    if partitions.get("factory") != (0, 0, 0x20000, 0x300000):
        raise ValueError("build factory partition does not match the Rev 2 profile")
    app = images[0x20000].read_bytes()
    if len(app) > 0x300000 or len(app) < 80 or struct.unpack_from("<I", app, 32)[0] != 0xABCD5432:
        raise ValueError("invalid or oversized application image")
    if app[48:80].split(b"\0", 1)[0].decode("ascii") != version:
        raise ValueError("application version does not match requested manifest version")
    result = {"schema_version": 1, "board": "waveshare_touch_lcd_185c_box_rev2", "firmware_version": version,
              "flash_size": 0x1000000, "images": [],
              "nvs": {"offset": 0x9000, "size": 0x10000, "bundle_schema_version": 1}}
    contents = []
    for offset, name, limit in ((0, "bootloader.bin", 0x8000), (0x8000, "partition-table.bin", 0x1000),
                                (0x19000, "ota-data.bin", 0x2000),
                                (0x20000, "application.bin", 0x300000)):
        data = images[offset].read_bytes()
        if not data or len(data) > limit:
            raise ValueError(f"{name} does not fit its flash region")
        contents.append((name, data))
        result["images"].append({"file": name, "offset": offset, "size": len(data),
                                 "sha256": hashlib.sha256(data).hexdigest()})
    destination.mkdir(parents=True, exist_ok=False)
    for name, data in contents:
        (destination / name).write_bytes(data)
    (destination / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    export(args.build, args.output, args.version)


if __name__ == "__main__":
    main()
