#!/usr/bin/env python3
"""Argus K10 attendance event receiver.

The Volcano ConversationalAI Embedded Kit 2.0 connects from the K10 directly to
Volcano with InstanceID/ProductKey/ProductSecret. This bridge therefore only
receives attendance events; it never forwards audio and never holds Volcano
cloud credentials.
"""
from __future__ import annotations

import argparse
import hmac
import json
import logging
import os
import socketserver
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from typing import Tuple
from urllib.parse import urlsplit

LOG = logging.getLogger("argus-bridge")
MAX_BODY_BYTES = 16 * 1024


@dataclass(frozen=True)
class BridgeConfig:
    listen_host: str
    port: int
    device_token: str
    data_dir: Path

    @classmethod
    def from_env(cls, env: dict[str, str]) -> "BridgeConfig":
        try:
            port = int(env.get("ARGUS_BRIDGE_PORT", "8787"))
        except ValueError as exc:
            raise ValueError("ARGUS_BRIDGE_PORT must be an integer") from exc
        if not 1 <= port <= 65535:
            raise ValueError("ARGUS_BRIDGE_PORT must be between 1 and 65535")
        token = env.get("ARGUS_BRIDGE_TOKEN", "").strip()
        return cls(
            listen_host=env.get("ARGUS_BRIDGE_HOST", "0.0.0.0").strip(),
            port=port,
            device_token=token,
            data_dir=Path(env.get("ARGUS_DATA_DIR", "./data")).expanduser(),
        )

    def health(self) -> dict:
        return {
            "service": "argus-attendance-bridge",
            "ok": len(self.device_token) >= 24,
            "voice_architecture": "volc-conversational-ai-embedded-kit-2.0-low-load-ws",
        }


def load_env(path: Path) -> dict[str, str]:
    values = dict(os.environ)
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


class AttendanceLog:
    def __init__(self, data_dir: Path) -> None:
        self.path = data_dir / "attendance.jsonl"
        self.lock = threading.Lock()

    def append(self, event: dict) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        line = json.dumps(event, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        with self.lock, self.path.open("a", encoding="utf-8") as stream:
            stream.write(line + "\n")
            stream.flush()


class ArgusHandler(BaseHTTPRequestHandler):
    server_version = "ArgusAttendanceBridge/2"
    protocol_version = "HTTP/1.1"

    @property
    def bridge(self) -> "ArgusServer":
        return self.server  # type: ignore[return-value]

    def log_message(self, fmt: str, *args: object) -> None:
        LOG.info("%s - %s", self.client_address[0], fmt % args)

    def send_json(self, status: HTTPStatus, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, status: HTTPStatus, message: str) -> None:
        self.send_json(status, {"error": message})

    def authorized(self) -> bool:
        supplied = self.headers.get("Authorization", "")
        return bool(self.bridge.config.device_token and supplied.startswith("Bearer ") and
                    hmac.compare_digest(supplied[7:], self.bridge.config.device_token))

    def read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("Invalid Content-Length") from exc
        if length <= 0 or length > MAX_BODY_BYTES:
            raise ValueError("Request body is empty or too large")
        try:
            value = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("Request body is not valid JSON") from exc
        if not isinstance(value, dict):
            raise ValueError("Request body must be a JSON object")
        return value

    def do_GET(self) -> None:
        if urlsplit(self.path).path != "/health":
            self.send_error_json(HTTPStatus.NOT_FOUND, "Not found")
            return
        self.send_json(HTTPStatus.OK, self.bridge.config.health())

    def do_POST(self) -> None:
        if urlsplit(self.path).path != "/attendance/event":
            self.send_error_json(HTTPStatus.NOT_FOUND, "Not found")
            return
        if not self.authorized():
            self.send_error_json(HTTPStatus.UNAUTHORIZED, "Invalid device token")
            return
        try:
            payload = self.read_json()
            event = self.bridge.record_attendance(payload)
            self.send_json(HTTPStatus.ACCEPTED, {"data": {"accepted": True, "event": event}})
        except ValueError as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))
        except Exception:
            LOG.exception("Unhandled attendance event error")
            self.send_error_json(HTTPStatus.INTERNAL_SERVER_ERROR, "Internal bridge error")


class ArgusServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: Tuple[str, int], config: BridgeConfig) -> None:
        self.config = config
        self.attendance = AttendanceLog(config.data_dir)
        super().__init__(address, ArgusHandler)

    def record_attendance(self, payload: dict) -> dict:
        required = ("event_id", "person_id", "device_id", "timestamp")
        event: dict[str, object] = {}
        for key in required:
            value = payload.get(key)
            if not isinstance(value, str) or not value.strip() or len(value) > 64:
                raise ValueError(f"{key} must be a non-empty string of at most 64 characters")
            event[key] = value.strip()
        kind = payload.get("kind", "check_in")
        if not isinstance(kind, str) or len(kind) > 32:
            raise ValueError("kind must be a string of at most 32 characters")
        try:
            confidence = float(payload.get("confidence", 0.0))
        except (TypeError, ValueError) as exc:
            raise ValueError("confidence must be numeric") from exc
        event["kind"] = kind.strip() or "check_in"
        event["confidence"] = max(0.0, min(1.0, confidence))
        event["local_only"] = bool(payload.get("local_only", False))
        event["received_at"] = int(time.time())
        self.attendance.append(event)
        LOG.info("Attendance person=%s event=%s", event["person_id"], event["event_id"])
        return event


def main() -> None:
    parser = argparse.ArgumentParser(description="Argus K10 attendance bridge")
    parser.add_argument("--env", default=str(Path(__file__).with_name(".env")))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    config = BridgeConfig.from_env(load_env(Path(args.env)))
    if args.check:
        health = config.health()
        print(json.dumps(health, ensure_ascii=False, indent=2, sort_keys=True))
        raise SystemExit(0 if health["ok"] else 2)
    if len(config.device_token) < 24:
        raise SystemExit("ARGUS_BRIDGE_TOKEN must be at least 24 characters")

    server = ArgusServer((config.listen_host, config.port), config)
    LOG.info("Attendance bridge listening on http://%s:%d", config.listen_host, config.port)
    LOG.info("Attendance log: %s", (config.data_dir / "attendance.jsonl").resolve())
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        LOG.info("Stopping attendance bridge")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
