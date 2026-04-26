"""Shared guard for GUI-driving dev commands."""

from __future__ import annotations

import ctypes
import json
import os
import sys
import time
from pathlib import Path

from .paths import ROOT


LOCK_PATH = ROOT / "build" / "void_player_gui_test.lock"


def _pid_is_running(pid: int) -> bool:
    if pid <= 0:
        return False

    if os.name == "nt":
        process_query_limited_information = 0x1000
        synchronize = 0x00100000
        still_active = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(
            process_query_limited_information | synchronize,
            False,
            pid,
        )
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return False
            return exit_code.value == still_active
        finally:
            kernel32.CloseHandle(handle)

    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _read_lock(path: Path) -> dict[str, object]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 - malformed stale locks should not block forever.
        return {}


class GuiTestLock:
    """Fail-fast lock for commands that launch real Flutter windows."""

    def __init__(self, owner: str) -> None:
        self.owner = owner
        self.path = LOCK_PATH
        self._acquired = False

    def __enter__(self) -> "GuiTestLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        while True:
            try:
                fd = os.open(
                    self.path,
                    os.O_CREAT | os.O_EXCL | os.O_WRONLY,
                )
            except FileExistsError:
                info = _read_lock(self.path)
                pid = int(info.get("pid") or 0)
                if not _pid_is_running(pid):
                    try:
                        self.path.unlink()
                    except FileNotFoundError:
                        pass
                    continue

                owner = info.get("owner") or "unknown"
                command = info.get("command") or "unknown command"
                created_at = info.get("created_at") or "unknown time"
                raise RuntimeError(
                    "another GUI/window test is already running:\n"
                    f"  owner: {owner}\n"
                    f"  pid: {pid}\n"
                    f"  command: {command}\n"
                    f"  created_at: {created_at}\n"
                    f"Remove {self.path} only if that process is gone."
                )

            try:
                payload = {
                    "owner": self.owner,
                    "pid": os.getpid(),
                    "command": " ".join(sys.argv),
                    "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                }
                os.write(fd, json.dumps(payload, ensure_ascii=False).encode("utf-8"))
                self._acquired = True
                return self
            finally:
                os.close(fd)

    def __exit__(self, _exc_type, _exc, _tb) -> None:
        if not self._acquired:
            return
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def gui_test_lock(owner: str) -> GuiTestLock:
    return GuiTestLock(owner)
