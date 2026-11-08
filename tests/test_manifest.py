"""Tests for the public firmware artifact exporter."""

import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("firmware_manifest", ROOT / "tools/firmware_manifest.py")
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.output = self.root / "release"
        table = b"".join([
            struct.pack("<HBBII16sI", 0x50AA, 1, 2, 0x9000, 0x10000, b"nvs", 0),
            struct.pack("<HBBII16sI", 0x50AA, 1, 0, 0x19000, 0x2000, b"otadata", 0),
            struct.pack("<HBBII16sI", 0x50AA, 0, 0, 0x20000, 0x300000, b"factory", 0),
        ]) + b"\xff" * 32
        app = bytearray(128)
        struct.pack_into("<I", app, 32, 0xABCD5432)
        app[48:54] = b"1.0.0\0"
        for name, data in (("boot.bin", b"boot"), ("partition.bin", table), ("ota.bin", b"ota"), ("app.bin", app)):
            (self.build / name).write_bytes(data)
        (self.build / "flasher_args.json").write_text(json.dumps({"flash_files": {
            "0x0": "boot.bin", "0x8000": "partition.bin", "0x19000": "ota.bin", "0x20000": "app.bin"}}))

    def test_export_checksums_offsets_and_dynamic_nvs(self):
        result = manifest.export(self.build, self.output, "1.0.0")
        self.assertEqual([entry["offset"] for entry in result["images"]], [0, 0x8000, 0x19000, 0x20000])
        self.assertEqual(result["nvs"]["size"], 0x10000)
        for image in result["images"]:
            data = (self.output / image["file"]).read_bytes()
            self.assertEqual(image["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(image["size"], len(data))
        self.assertFalse((self.output / "nvs.bin").exists())

    def test_rejects_version_or_partition_mismatch(self):
        with self.assertRaises(ValueError):
            manifest.export(self.build, self.output, "2.0.0")
        table = bytearray((self.build / "partition.bin").read_bytes())
        struct.pack_into("<I", table, 8, 0x6000)
        (self.build / "partition.bin").write_bytes(table)
        with self.assertRaises(ValueError):
            manifest.export(self.build, self.output, "1.0.0")

    def test_published_release_matches_manifest(self):
        release = ROOT / "firmware" / "waveshare-rev2" / "0.1.0"
        data = json.loads((release / "manifest.json").read_text())
        self.assertEqual(data["firmware_version"], "0.1.0")
        self.assertEqual(data["nvs"]["offset"], 0x9000)
        self.assertEqual(data["nvs"]["size"], 0x10000)
        self.assertEqual([image["offset"] for image in data["images"]], [0, 0x8000, 0x19000, 0x20000])
        for image in data["images"]:
            content = (release / image["file"]).read_bytes()
            self.assertEqual(len(content), image["size"])
            self.assertEqual(hashlib.sha256(content).hexdigest(), image["sha256"])
