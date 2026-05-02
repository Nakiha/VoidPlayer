"""Standalone analysis window resize stress regression."""

from __future__ import annotations

import ctypes
import json
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

from .paths import ROOT, app_exe_path
from .ui_lock import gui_test_lock


WM_CLOSE = 0x0010
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
FLUTTER_WINDOW_CLASS = "FLUTTER_RUNNER_WIN32_WINDOW"


EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32 = ctypes.windll.user32
user32.EnumWindows.argtypes = [EnumWindowsProc, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.SetWindowPos.argtypes = [
    wintypes.HWND,
    wintypes.HWND,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_uint,
]
user32.SetWindowPos.restype = wintypes.BOOL
user32.PostMessageW.argtypes = [
    wintypes.HWND,
    ctypes.c_uint,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.PostMessageW.restype = wintypes.BOOL


def _release_dir(debug: bool) -> Path:
    return ROOT / "build" / "windows" / "x64" / "runner" / ("Debug" if debug else "Release")


def _logs_dir(debug: bool) -> Path:
    return _release_dir(debug) / "logs"


def _cache_dir(debug: bool) -> Path:
    return _release_dir(debug) / "cache"


def _find_flutter_window(pid: int, timeout_s: float = 10.0) -> int:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        hwnds: list[int] = []

        @EnumWindowsProc
        def enum_proc(hwnd: int, _lparam: int) -> bool:
            proc_id = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(proc_id))
            if proc_id.value == pid:
                cls = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, cls, len(cls))
                if cls.value == FLUTTER_WINDOW_CLASS:
                    hwnds.append(hwnd)
            return True

        user32.EnumWindows(enum_proc, 0)
        if hwnds:
            return hwnds[0]
        time.sleep(0.1)
    raise RuntimeError(f"Timed out waiting for Flutter window for pid={pid}")


def _existing_crash_logs(debug: bool) -> set[Path]:
    logs = _logs_dir(debug)
    if not logs.exists():
        return set()
    return set(logs.glob("crash_*.log"))


def _pick_analysis_hash(debug: bool, requested_hash: str | None) -> tuple[str, str | None]:
    if requested_hash:
        return requested_hash, None

    cache = _cache_dir(debug)
    index_path = cache / "analysis_index.json"
    if not index_path.exists():
        raise RuntimeError(
            f"Analysis cache index not found: {index_path}\n"
            "Run an analysis UI test or open analysis once before this regression."
        )

    index = json.loads(index_path.read_text(encoding="utf-8"))
    entries = index.get("entries", {})
    if not isinstance(entries, dict) or not entries:
        raise RuntimeError(f"Analysis cache index has no entries: {index_path}")

    def usable_items():
        for key, value in entries.items():
            if not isinstance(key, str) or not isinstance(value, dict):
                continue
            if not (cache / f"{key}.vbi").exists() or not (cache / f"{key}.vbt").exists():
                continue
            yield key, value

    items = list(usable_items())
    if not items:
        raise RuntimeError(f"No usable analysis cache entries in {cache}")

    # Prefer entries with VBS3 because the pyramid view is the historically
    # riskiest resize path.
    items.sort(key=lambda item: (not (cache / f"{item[0]}.vbs3").exists(), item[0]))
    chosen_hash, meta = items[0]
    file_name = meta.get("name") if isinstance(meta.get("name"), str) else None
    return chosen_hash, file_name


def run_analysis_resize_stress(
    *,
    debug: bool,
    build: bool,
    requested_hash: str | None,
    rounds: int,
    visible: bool,
) -> None:
    if build:
        from .flutter_app import flutter_build

        flutter_build(debug)

    exe = app_exe_path(debug)
    if not exe.exists():
        raise RuntimeError(f"exe not found: {exe}; run python dev.py build --flutter")

    analysis_hash, file_name = _pick_analysis_hash(debug, requested_hash)
    before_crashes = _existing_crash_logs(debug)

    cmd = [
        str(exe),
        "--standalone-analysis",
        f"--hash={analysis_hash}",
        "--x=140",
        "--y=140",
        "--width=1000",
        "--height=700",
        "--accentColor=4279976026",
    ]
    if file_name:
        cmd.append(f"--fileName={file_name}")
    if not visible:
        cmd.append("--silent-ui-test")

    print(f"Launching analysis resize stress: hash={analysis_hash}")
    proc = subprocess.Popen(cmd, cwd=str(ROOT))
    hwnd = 0
    try:
        hwnd = _find_flutter_window(proc.pid)
        sizes = [
            (1000, 700),
            (760, 420),
            (520, 320),
            (1200, 780),
            (430, 310),
            (1500, 900),
            (410, 300),
            (1000, 700),
            (600, 650),
            (1300, 360),
            (450, 850),
            (1000, 700),
        ]
        flags = SWP_NOZORDER | SWP_NOACTIVATE
        for _round in range(rounds):
            for width, height in sizes:
                if proc.poll() is not None:
                    raise RuntimeError(
                        f"analysis process exited during resize: code={proc.returncode}"
                    )
                ok = user32.SetWindowPos(hwnd, 0, 140, 140, width, height, flags)
                if not ok:
                    raise RuntimeError(f"SetWindowPos failed for {width}x{height}")
                time.sleep(0.08)

        time.sleep(1.0)
        if proc.poll() is not None:
            raise RuntimeError(f"analysis process exited after resize: code={proc.returncode}")

        after_crashes = _existing_crash_logs(debug)
        new_crashes = sorted(after_crashes - before_crashes)
        if new_crashes:
            raise RuntimeError(
                "new crash log(s) generated during resize stress:\n"
                + "\n".join(str(path) for path in new_crashes)
            )

        print("Analysis resize stress passed.")
    finally:
        if proc.poll() is None:
            if hwnd:
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
            else:
                proc.kill()


def cmd_analysis_resize_stress(args) -> None:
    try:
        with gui_test_lock("analysis-resize-stress"):
            run_analysis_resize_stress(
                debug=args.debug,
                build=args.build,
                requested_hash=args.hash,
                rounds=args.rounds,
                visible=args.visible,
            )
    except Exception as exc:  # noqa: BLE001 - dev command should print concise failures.
        print(f"Analysis resize stress failed: {exc}")
        sys.exit(1)
