#!/usr/bin/env python3
"""Validate a device bundle and generate its ESP-IDF NVS image."""

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlsplit


class BundleError(ValueError):
    pass


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise BundleError(f"{path} must contain a JSON object")
    return value


def require_keys(value: dict, keys: set[str], name: str) -> None:
    actual = set(value)
    if actual != keys:
        missing = ", ".join(sorted(keys - actual))
        extra = ", ".join(sorted(actual - keys))
        details = ", ".join(part for part in (
            f"missing: {missing}" if missing else "",
            f"unknown: {extra}" if extra else "",
        ) if part)
        raise BundleError(f"{name} has invalid fields ({details})")


def require_string(value: object, name: str, maximum: int) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise BundleError(f"{name} must be a non-empty string of at most {maximum} characters")
    return value


def require_integer(value: object, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise BundleError(f"{name} must be an integer from {minimum} to {maximum}")
    return value


def validate(config: dict, credentials: dict) -> list[tuple[str, str, str]]:
    config_keys = {"schema_version", "device", "server", "audio", "music"}
    credential_keys = {"schema_version", "wifi", "fulloch"}
    optional_config_keys = {"sonos"} if "sonos" in config else set()
    require_keys(config, config_keys | optional_config_keys, "config")
    require_keys(credentials, credential_keys | ({"spotify"} if "spotify" in credentials else set()), "credentials")
    if config["schema_version"] != 1 or credentials["schema_version"] != 1:
        raise BundleError("bundle schema_version must be 1")

    device = config["device"]
    server = config["server"]
    audio = config["audio"]
    wifi = credentials["wifi"]
    fulloch = credentials["fulloch"]
    for value, keys, name in (
        (device, {"id", "name", "timezone"}, "device"),
        (server, {"url"}, "server"),
        (audio, {"speaker_volume_percent", "aec_enabled"}, "audio"),
        (wifi, {"ssid", "password"}, "wifi"),
        (fulloch, {"device_token"}, "fulloch"),
    ):
        if not isinstance(value, dict):
            raise BundleError(f"{name} must be an object")
        require_keys(value, keys, name)

    if "spotify" in credentials:
        spotify_credentials = credentials["spotify"]
        if not isinstance(spotify_credentials, dict):
            raise BundleError("spotify credentials must be an object")
        require_keys(spotify_credentials, {"client_id", "client_secret", "refresh_token"}, "spotify credentials")
        require_string(spotify_credentials["client_id"], "spotify.client_id", 256)
        require_string(spotify_credentials["client_secret"], "spotify.client_secret", 256)
        require_string(spotify_credentials["refresh_token"], "spotify.refresh_token", 512)
    music = config["music"]
    if not isinstance(music, dict):
        raise BundleError("music must be an object")
    require_keys(music, {"player"}, "music")
    if music["player"] not in {"spotify", "sonos"}:
        raise BundleError("music.player must be spotify or sonos")
    if ("sonos" in config) != ("music" in config and config["music"]["player"] == "sonos"):
        raise BundleError("sonos configuration requires music.player set to sonos")
    if ("spotify" in credentials) != (music["player"] == "spotify"):
        raise BundleError("Spotify credentials require music.player set to spotify")
    if "sonos" in config:
        sonos = config["sonos"]
        if not isinstance(sonos, dict):
            raise BundleError("sonos must be an object")
        require_keys(sonos, {"host"}, "sonos")
        require_string(sonos["host"], "sonos.host", 63)

    server_url = require_string(server["url"], "server.url", 256)
    parsed_url = urlsplit(server_url)
    if parsed_url.scheme != "wss" or not parsed_url.hostname or parsed_url.username or parsed_url.password:
        raise BundleError("server.url must be a wss:// URL without user credentials")
    if not isinstance(audio["aec_enabled"], bool):
        raise BundleError("audio.aec_enabled must be a boolean")

    values = [
        ("device", "id", require_string(device["id"], "device.id", 64)),
        ("device", "name", require_string(device["name"], "device.name", 64)),
        ("device", "timezone", require_string(device["timezone"], "device.timezone", 64)),
        ("server", "url", server_url),
        ("audio", "speaker_volume", str(require_integer(audio["speaker_volume_percent"], "audio.speaker_volume_percent", 0, 100))),
        ("audio", "aec_enabled", str(audio["aec_enabled"]).lower()),
        ("wifi", "ssid", require_string(wifi["ssid"], "wifi.ssid", 32)),
        ("wifi", "password", require_string(wifi["password"], "wifi.password", 63)),
        ("fulloch", "device_token", require_string(fulloch["device_token"], "fulloch.device_token", 256)),
    ]
    if "spotify" in credentials:
        values.extend([
            ("spotify", "client_id", spotify_credentials["client_id"]),
            ("spotify", "client_secret", spotify_credentials["client_secret"]),
            ("spotify", "refresh_token", spotify_credentials["refresh_token"]),
        ])
    if "music" in config:
        values.append(("music", "player", config["music"]["player"]))
    if "sonos" in config:
        values.append(("sonos", "host", config["sonos"]["host"]))
    return [(namespace, key, value) for namespace, key, value in values]


def write_csv(entries: list[tuple[str, str, str]], output: Path) -> None:
    with output.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(("key", "type", "encoding", "value"))
        namespace = None
        for entry_namespace, key, value in entries:
            if entry_namespace != namespace:
                writer.writerow((entry_namespace, "namespace", "", ""))
                namespace = entry_namespace
            writer.writerow((key, "data", "string", value))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("validate", "stage"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--config", required=True, type=Path)
        command_parser.add_argument("--credentials", required=True, type=Path)
        if command == "stage":
            command_parser.add_argument("--output", required=True, type=Path)
            command_parser.add_argument("--nvs-generator", required=True, type=Path)
            command_parser.add_argument("--size", required=True)
    args = parser.parse_args()

    try:
        entries = validate(load_json(args.config), load_json(args.credentials))
        if args.command == "stage":
            if not args.nvs_generator.is_file():
                raise BundleError(f"NVS generator not found: {args.nvs_generator}")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory() as directory:
                csv_path = Path(directory) / "nvs.csv"
                write_csv(entries, csv_path)
                subprocess.run(
                    [sys.executable, str(args.nvs_generator), "generate", str(csv_path), str(args.output), args.size],
                    check=True,
                )
    except (BundleError, subprocess.CalledProcessError) as error:
        print(f"provisioning error: {error}", file=sys.stderr)
        return 1
    print("provisioning bundle is valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
