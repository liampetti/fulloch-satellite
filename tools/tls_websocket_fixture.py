#!/usr/bin/env python3
"""Small TLS WebSocket fixture for satellite-v2 normal and adverse sequences."""

import argparse
import base64
import hashlib
import json
import socket
import ssl
from typing import Any


WELCOME = {
    "type": "satellite.welcome",
    "session_id": "fixture-session",
    "protocol": {"major": 2, "minor": 3},
    "audio": {
        "uplink": {"encoding": "pcm_s16le", "sample_rate_hz": 16000, "channels": 1, "frame_duration_ms": 20},
        "downlink": {"encoding": "pcm_s16le", "sample_rate_hz": 16000, "channels": 1},
    },
    "heartbeat_interval_ms": 1000,
}


def validate_hello(message: dict[str, Any]) -> bool:
    protocol = message.get("protocol")
    device = message.get("device")
    capabilities = message.get("capabilities")
    resume = message.get("resume")
    resume_valid = resume is None or (
        isinstance(resume, dict)
        and all(isinstance(resume.get(field), str) and resume[field] for field in ("session_id", "turn_id"))
        and isinstance(resume.get("next_seq"), int)
        and resume["next_seq"] >= 0
    )
    return (
        message.get("type") == "satellite.hello"
        and isinstance(message.get("token"), str)
        and bool(message["token"])
        and protocol == {"name": "satellite-v2", "major": 2, "minor": 3}
        and isinstance(device, dict)
        and all(isinstance(device.get(field), str) and device[field] for field in ("id", "name", "firmware", "build", "board"))
        and isinstance(capabilities, dict)
        and all(capabilities.get(field) is True for field in ("audio_input", "audio_output", "display", "touch"))
        and resume_valid
    )


def scenario_messages(name: str) -> list[dict[str, Any]]:
    if name == "normal":
        return [
            WELCOME,
            {"type": "satellite.health_request", "id": "fixture-health-1"},
            {"type": "assistant.state", "state": "wake_detected", "turn_id": "fixture-turn"},
            {"type": "assistant.state", "state": "listening", "turn_id": "fixture-turn"},
            {"type": "assistant.state", "state": "thinking", "turn_id": "fixture-turn"},
            {"type": "assistant.state", "state": "speaking", "turn_id": "fixture-turn"},
            {"type": "assistant.state", "state": "idle", "turn_id": None},
        ]
    if name == "malformed-welcome":
        return [{"type": "satellite.welcome", "protocol": {"major": 3, "minor": 1}}]
    if name == "stale-turn":
        return [WELCOME, {"type": "assistant.state", "state": "speaking", "turn_id": "stale"}]
    raise ValueError(f"unknown scenario: {name}")


def receive_http_request(connection: socket.socket) -> bytes:
    request = b""
    while b"\r\n\r\n" not in request:
        data = connection.recv(1024)
        if not data:
            raise ConnectionError("client closed before WebSocket upgrade")
        request += data
    return request


def websocket_upgrade(connection: socket.socket) -> None:
    request = receive_http_request(connection).decode("ascii")
    headers = dict(line.split(": ", 1) for line in request.split("\r\n")[1:] if ": " in line)
    key = headers["Sec-WebSocket-Key"]
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n"
    connection.sendall(response.encode("ascii"))


def receive_frame(connection: socket.socket) -> bytes:
    first, second = connection.recv(2)
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(connection.recv(2), "big")
    elif length == 127:
        length = int.from_bytes(connection.recv(8), "big")
    mask = connection.recv(4)
    payload = bytearray()
    while len(payload) < length:
        payload.extend(connection.recv(length - len(payload)))
    return bytes(value ^ mask[index % 4] for index, value in enumerate(payload))


def send_text_frame(connection: socket.socket, message: dict[str, Any]) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = bytes([0x81])
    if len(payload) < 126:
        header += bytes([len(payload)])
    else:
        header += bytes([126]) + len(payload).to_bytes(2, "big")
    connection.sendall(header + payload)


def serve_once(cert: str, key: str, port: int, scenario: str) -> None:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    with socket.create_server(("127.0.0.1", port), reuse_port=False) as listener:
        print(f"fixture listening on wss://127.0.0.1:{listener.getsockname()[1]}", flush=True)
        with context.wrap_socket(listener.accept()[0], server_side=True) as connection:
            websocket_upgrade(connection)
            hello = json.loads(receive_frame(connection))
            if not validate_hello(hello):
                raise ValueError("client did not send a valid satellite.hello first")
            for message in scenario_messages(scenario):
                send_text_frame(connection, message)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert", required=True, help="fixture server certificate")
    parser.add_argument("--key", required=True, help="fixture server private key")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--scenario", choices=("normal", "malformed-welcome", "stale-turn"), default="normal")
    args = parser.parse_args()
    serve_once(args.cert, args.key, args.port, args.scenario)


if __name__ == "__main__":
    main()
