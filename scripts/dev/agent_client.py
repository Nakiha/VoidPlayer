"""Agent protocol client and end-to-end smoke.

The client side of lib/docs/AGENT_PROTOCOL.md: launch VoidPlayer with
--agent-connection-file, poll the connection file for the loopback port and
token, then drive the session over line-delimited JSON. Usable as a library
from agent scripts and as `python dev.py agent ...` verbs.
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

AGENT_PROTOCOL_VERSION = 1


class AgentProtocolError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


class VoidPlayerAgentClient:
    """Blocking line-JSON client for one VoidPlayer agent session."""

    def __init__(self, sock: socket.socket):
        self._socket = sock
        self._reader = sock.makefile("r", encoding="utf-8")
        self._next_id = 1

    # -- connection ---------------------------------------------------------

    @classmethod
    def connect_from_file(
        cls,
        connection_file: Path,
        timeout_seconds: float = 30.0,
    ) -> "VoidPlayerAgentClient":
        info = cls._poll_connection_file(connection_file, timeout_seconds)
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        sock.settimeout(15)
        client = cls(sock)
        client._send({"type": "hello", "token": info["token"]})
        ack = client._read_message()
        if ack.get("type") != "helloAck":
            raise RuntimeError(f"unexpected handshake response: {ack}")
        if ack.get("protocolVersion") != AGENT_PROTOCOL_VERSION:
            raise RuntimeError(
                "protocol version mismatch: "
                f"client {AGENT_PROTOCOL_VERSION}, app {ack.get('protocolVersion')}"
            )
        return client

    @staticmethod
    def _poll_connection_file(connection_file: Path, timeout_seconds: float) -> dict:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            if connection_file.exists():
                try:
                    info = json.loads(connection_file.read_text(encoding="utf-8"))
                    if "port" in info and "token" in info:
                        return info
                except (json.JSONDecodeError, OSError):
                    pass
            time.sleep(0.2)
        raise TimeoutError(f"agent connection file never appeared: {connection_file}")

    def close(self) -> None:
        try:
            self._reader.close()
        finally:
            self._socket.close()

    # -- protocol -----------------------------------------------------------

    def request(self, method: str, params: dict | None = None) -> dict:
        request_id = self._next_id
        self._next_id += 1
        self._send({"id": request_id, "method": method, "params": params or {}})
        while True:
            message = self._read_message()
            if message.get("id") != request_id:
                continue
            if "error" in message:
                error = message["error"] or {}
                raise AgentProtocolError(
                    str(error.get("code", "unknown")),
                    str(error.get("message", "")),
                )
            return message.get("result") or {}

    def _send(self, message: dict) -> None:
        self._socket.sendall((json.dumps(message) + "\n").encode("utf-8"))

    def _read_message(self) -> dict:
        line = self._reader.readline()
        if not line:
            raise ConnectionError("agent connection closed by the app")
        return json.loads(line)

    # -- verbs --------------------------------------------------------------

    def get_session(self) -> dict:
        return self.request("getSession")

    def get_marks(self) -> dict:
        return self.request("getMarks")

    def export_marks(self, path: str) -> dict:
        return self.request("exportMarks", {"path": path})

    def play(self) -> None:
        self.request("play")

    def pause(self) -> None:
        self.request("pause")

    def seek_to(self, pts_us: int) -> None:
        self.request("seekTo", {"ptsUs": pts_us})

    def add_media(self, path: str) -> dict:
        return self.request("addMedia", {"path": path})

    def set_media_source_id(self, slot_index: int, source_id: str) -> None:
        self.request(
            "setMediaSourceId",
            {"slotIndex": slot_index, "sourceId": source_id},
        )

    def wait_for_media(self, count: int, timeout_seconds: float = 30.0) -> dict:
        """Polls getSession until at least `count` tracks are loaded."""
        deadline = time.monotonic() + timeout_seconds
        while True:
            session = self.get_session()
            if len(session.get("media", [])) >= count:
                return session
            if time.monotonic() > deadline:
                raise TimeoutError(
                    f"expected {count} media, got {len(session.get('media', []))}"
                )
            time.sleep(0.3)


# -- dev.py commands --------------------------------------------------------

_VERBS = ("session", "marks", "export", "play", "pause", "seek", "set-source-id")


def cmd_agent(args) -> None:
    """Run one agent protocol verb against a running VoidPlayer instance."""
    client = VoidPlayerAgentClient.connect_from_file(
        Path(args.connection_file),
        timeout_seconds=args.timeout,
    )
    try:
        if args.verb == "add-media":
            _require(args.path, "add-media needs --path")
            print(json.dumps(client.add_media(args.path), indent=2))
        elif args.verb == "session":
            print(json.dumps(client.get_session(), indent=2))
        elif args.verb == "marks":
            print(json.dumps(client.get_marks(), indent=2))
        elif args.verb == "export":
            _require(args.path, "export needs --path")
            print(json.dumps(client.export_marks(args.path), indent=2))
        elif args.verb == "play":
            client.play()
        elif args.verb == "pause":
            client.pause()
        elif args.verb == "seek":
            _require(args.pts_us is not None, "seek needs --pts-us")
            client.seek_to(args.pts_us)
        elif args.verb == "set-source-id":
            _require(
                args.slot is not None and args.source_id,
                "set-source-id needs --slot and --source-id",
            )
            client.set_media_source_id(args.slot, args.source_id)
        else:
            raise SystemExit(f"unknown verb: {args.verb}")
    finally:
        client.close()


def _require(condition, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def cmd_agent_smoke(args) -> None:
    """End-to-end agent protocol smoke against the real macOS app."""
    from . import flutter_app

    if not flutter_app._is_macos():
        print("agent-smoke currently supports macOS only")
        sys.exit(1)

    from .paths import macos_app_bundle_path, macos_app_exe_path

    app_exe = macos_app_exe_path(args.debug)
    app_bundle = macos_app_bundle_path(args.debug)
    if args.build or not app_bundle.exists():
        flutter_app.flutter_build_macos(args.debug)
    if not app_exe.exists():
        print(f"ERROR: macOS app not found: {app_bundle}")
        sys.exit(1)
    flutter_app._install_macos_ffmpeg_analyzer(app_bundle)
    flutter_app._codesign_macos_app_bundle(app_bundle)
    flutter_app._register_macos_app_bundle(app_bundle)

    container_scripts_dir, container_media_dir = (
        flutter_app._macos_ui_test_container_dirs()
    )
    connection_file = container_scripts_dir / "agent_connection.json"
    connection_file.unlink(missing_ok=True)
    export_path = container_scripts_dir / "agent_smoke_marks.json"
    export_path.unlink(missing_ok=True)

    # Generate one deterministic media file inside the app container so the
    # sandboxed app can read it; the smoke loads it through the protocol.
    media_path = container_media_dir / "build/generated/macos/agent_smoke.mp4"
    if not media_path.exists():
        flutter_app._generate_macos_test_video(
            ["0.0", "GENERATE_TEST_VIDEO", "x", "120", "30", "640", "360"],
            str(media_path),
        )

    cmd = [
        str(app_exe),
        f"--agent-connection-file={connection_file}",
    ]
    if args.log_level:
        cmd.append(f"--log-level={args.log_level}")

    print(f"Launching {app_exe}")
    process = subprocess.Popen(cmd)
    failures: list[str] = []
    try:
        client = VoidPlayerAgentClient.connect_from_file(connection_file)
        try:
            result = client.add_media(str(media_path))
            print(f"addMedia: trackCount={result.get('trackCount')}")
            session = client.wait_for_media(1)
            media = session["media"][0]
            print(f"session: media={media['path']} hash={media['mediaHash']}")

            client.set_media_source_id(0, "agent_smoke_clip")
            session = client.get_session()
            if session["media"][0].get("sourceId") != "agent_smoke_clip":
                failures.append("setMediaSourceId did not round-trip")

            client.play()
            time.sleep(0.8)
            if not client.get_session()["playback"]["isPlaying"]:
                failures.append("play did not start playback")
            client.pause()

            marks = client.get_marks()
            if marks.get("version") != 1:
                failures.append(f"unexpected marks version: {marks.get('version')}")

            client.export_marks(str(export_path))
            if not export_path.exists():
                failures.append(f"export file missing: {export_path}")
            else:
                document = json.loads(export_path.read_text(encoding="utf-8"))
                if document.get("media", [{}])[0].get("sourceId") != "agent_smoke_clip":
                    failures.append("exported document is missing the source id")

            try:
                client.request("definitelyNotAMethod")
                failures.append("unknown method did not raise")
            except AgentProtocolError as error:
                if error.code != "unknownMethod":
                    failures.append(f"unexpected error code: {error.code}")
        finally:
            client.close()
    except Exception as exc:  # noqa: BLE001 - smoke surfaces everything
        failures.append(str(exc))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()

    if failures:
        print("\nAgent smoke FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        sys.exit(1)
    print("\nAgent smoke passed.")
