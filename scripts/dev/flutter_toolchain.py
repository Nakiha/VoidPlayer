"""Pinned Flutter SDK toolchain checks for VoidPlayer development commands."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .paths import ROOT
from .process import header, run


LOCK_PATH = ROOT / "toolchains" / "flutter.lock.json"


@dataclass(frozen=True)
class MacOSLocalEngineArtifact:
    engine: str
    host: str
    asset: str
    sha256: str


@dataclass(frozen=True)
class FlutterToolchainLock:
    name: str
    upstream_flutter: str
    flutter_version: str
    dart_sdk_version: str
    framework_revision: str
    engine_revision: str
    fork_remote: str
    fork_ref: str
    fork_branch: str
    fork_commit: str
    default_install_path: Path
    macos_local_engine_artifacts: dict[str, MacOSLocalEngineArtifact]
    required_patch_markers: tuple[dict[str, str], ...]


def load_flutter_toolchain_lock() -> FlutterToolchainLock:
    with LOCK_PATH.open("r", encoding="utf-8") as file:
        raw = json.load(file)

    if not isinstance(raw, dict):
        raise RuntimeError(f"{LOCK_PATH}: root value must be an object")

    def require_string(key: str) -> str:
        value = raw.get(key)
        if not isinstance(value, str) or not value:
            raise RuntimeError(f"{LOCK_PATH}: missing string field {key!r}")
        return value

    markers = raw.get("requiredPatchMarkers", [])
    if not isinstance(markers, list):
        raise RuntimeError(f"{LOCK_PATH}: requiredPatchMarkers must be a list")
    checked_markers: list[dict[str, str]] = []
    for marker in markers:
        if not isinstance(marker, dict):
            raise RuntimeError(f"{LOCK_PATH}: marker entries must be objects")
        path = marker.get("path")
        contains = marker.get("contains")
        if not isinstance(path, str) or not isinstance(contains, str):
            raise RuntimeError(f"{LOCK_PATH}: marker path/contains must be strings")
        checked_markers.append({"path": path, "contains": contains})

    raw_artifacts = raw.get("macosLocalEngineArtifacts", {})
    if raw_artifacts is None:
        raw_artifacts = {}
    if not isinstance(raw_artifacts, dict):
        raise RuntimeError(f"{LOCK_PATH}: macosLocalEngineArtifacts must be an object")
    macos_artifacts: dict[str, MacOSLocalEngineArtifact] = {}
    for mode, spec in raw_artifacts.items():
        if not isinstance(mode, str) or not isinstance(spec, dict):
            raise RuntimeError(
                f"{LOCK_PATH}: macosLocalEngineArtifacts entries must be objects"
            )
        engine = spec.get("engine")
        host = spec.get("host", engine)
        asset = spec.get("asset")
        sha256 = spec.get("sha256")
        if not all(isinstance(value, str) and value for value in (engine, host, asset, sha256)):
            raise RuntimeError(
                f"{LOCK_PATH}: macosLocalEngineArtifacts.{mode} has invalid fields"
            )
        macos_artifacts[mode] = MacOSLocalEngineArtifact(
            engine=engine,
            host=host,
            asset=asset,
            sha256=sha256,
        )

    return FlutterToolchainLock(
        name=require_string("name"),
        upstream_flutter=require_string("upstreamFlutter"),
        flutter_version=require_string("flutterVersion"),
        dart_sdk_version=require_string("dartSdkVersion"),
        framework_revision=require_string("frameworkRevision"),
        engine_revision=require_string("engineRevision"),
        fork_remote=require_string("forkRemote"),
        fork_ref=require_string("forkRef"),
        fork_branch=require_string("forkBranch"),
        fork_commit=require_string("forkCommit"),
        default_install_path=ROOT / require_string("defaultInstallPath"),
        macos_local_engine_artifacts=macos_artifacts,
        required_patch_markers=tuple(checked_markers),
    )


def flutter_bin() -> str:
    override = os.environ.get("VOIDPLAYER_FLUTTER_BIN")
    if override:
        return override

    lock = load_flutter_toolchain_lock()
    pinned_bin = lock.default_install_path / "bin" / _flutter_executable_name()
    if pinned_bin.exists():
        return str(pinned_bin)
    return "flutter"


def local_engine_args() -> list[str]:
    engine_src = os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH")
    engine = os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE")
    engine_host = os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST")
    return _local_engine_args(engine_src, engine, engine_host)


def local_engine_args_for_mode(debug: bool) -> list[str]:
    lock = load_flutter_toolchain_lock()
    artifact = _macos_local_engine_artifact_for_mode(lock, debug)
    engine_src = str(_local_engine_src_path(lock))
    if debug:
        engine = os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE") or artifact.engine
        engine_host = (
            os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST") or artifact.host
        )
    else:
        engine = (
            os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE")
            or artifact.engine
            or _derive_release_engine_name(os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE"))
        )
        engine_host = (
            os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE")
            or artifact.host
            or _derive_release_engine_name(
                os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST")
            )
        )
    return _local_engine_args(engine_src, engine, engine_host)


def local_engine_name_for_mode(debug: bool) -> str | None:
    artifact = _macos_local_engine_artifact_for_mode(load_flutter_toolchain_lock(), debug)
    if debug:
        return os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE") or artifact.engine
    return (
        os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE")
        or artifact.engine
        or _derive_release_engine_name(os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE"))
    )


def local_engine_output_path(engine_name: str | None) -> Path | None:
    if not engine_name:
        return None
    return _local_engine_src_path(load_flutter_toolchain_lock()) / "out" / engine_name


def ensure_macos_local_engine_for_mode(debug: bool) -> None:
    if sys.platform != "darwin":
        return
    engine_name = local_engine_name_for_mode(debug)
    engine_path = local_engine_output_path(engine_name)
    if engine_path is not None and engine_path.exists():
        return
    bootstrap_macos_local_engines(modes=("debug" if debug else "release",))
    engine_path = local_engine_output_path(engine_name)
    if engine_path is None or not engine_path.exists():
        mode = "Debug" if debug else "Release"
        print(f"ERROR: macOS {mode} Flutter local engine is missing: {engine_path}")
        sys.exit(1)


def bootstrap_macos_local_engines(
    modes: tuple[str, ...] = ("debug", "release"),
    flutter_bin_path: Path | None = None,
) -> None:
    if sys.platform != "darwin":
        return
    lock = load_flutter_toolchain_lock()
    missing_modes = [
        mode
        for mode in modes
        if not (
            _local_engine_src_path(lock)
            / "out"
            / _macos_local_engine_artifact_for_mode(lock, mode == "debug").engine
        ).exists()
    ]
    if not missing_modes:
        return

    script = ROOT / "scripts" / "ci" / "bootstrap_flutter_macos_engine.sh"
    env = os.environ.copy()
    env["VOIDPLAYER_FLUTTER_BIN"] = str(
        flutter_bin_path
        or Path(
            env.get(
                "VOIDPLAYER_FLUTTER_BIN",
                str(lock.default_install_path / "bin" / _flutter_executable_name()),
            )
        )
    )
    env["VOIDPLAYER_FLUTTER_ENGINE_MODES"] = ",".join(missing_modes)
    header("Bootstrap macOS Flutter local engine")
    run([str(script)], cwd=str(ROOT), env=env)


def _local_engine_src_path(lock: FlutterToolchainLock) -> Path:
    override = os.environ.get("VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH")
    if override:
        return Path(override)

    flutter_bin_override = os.environ.get("VOIDPLAYER_FLUTTER_BIN")
    if flutter_bin_override:
        flutter_bin_path = Path(flutter_bin_override).expanduser()
        if flutter_bin_path.name == _flutter_executable_name():
            return flutter_bin_path.parent.parent / "engine" / "src"

    return lock.default_install_path / "engine" / "src"


def _macos_local_engine_artifact_for_mode(
    lock: FlutterToolchainLock,
    debug: bool,
) -> MacOSLocalEngineArtifact:
    mode = "debug" if debug else "release"
    artifact = lock.macos_local_engine_artifacts.get(mode)
    if artifact is None:
        raise RuntimeError(f"{LOCK_PATH}: missing macOS local engine artifact for {mode}")
    return artifact


def _derive_release_engine_name(engine: str | None) -> str | None:
    if not engine:
        return None
    if "debug_unopt" in engine:
        return engine.replace("debug_unopt", "release")
    if "debug" in engine:
        return engine.replace("debug", "release")
    return engine


def _local_engine_args(
    engine_src: str | None,
    engine: str | None,
    engine_host: str | None,
) -> list[str]:
    args: list[str] = []
    if engine_src:
        args.append(f"--local-engine-src-path={engine_src}")
    if engine:
        args.append(f"--local-engine={engine}")
    if engine_host:
        args.append(f"--local-engine-host={engine_host}")
    return args


def flutter_cmd(*args: str, local_engine: bool = False) -> list[str]:
    ensure_flutter_toolchain()
    cmd = [flutter_bin(), *args]
    if local_engine:
        cmd.extend(local_engine_args())
    return cmd


def ensure_flutter_toolchain() -> None:
    if os.environ.get("VOIDPLAYER_FLUTTER_TOOLCHAIN_CHECK") == "0":
        print(
            "WARNING: VOIDPLAYER_FLUTTER_TOOLCHAIN_CHECK=0; "
            "skipping Flutter SDK lock check."
        )
        return

    result = check_flutter_toolchain()
    if result.ok:
        return

    print("\nERROR: Flutter SDK does not match VoidPlayer's pinned toolchain.")
    for line in result.lines:
        print(f"  {line}")
    print("\nExpected:")
    print(f"  lock: {LOCK_PATH}")
    print(f"  fork: {result.lock.fork_remote}")
    print(f"  ref:  {result.lock.fork_ref}")
    print(f"  rev:  {result.lock.framework_revision}")
    print("\nFix:")
    print("  python dev.py toolchain bootstrap-flutter")
    print("  python dev.py toolchain doctor")
    print("\nOr point an existing checkout at the lock:")
    print(
        "  VOIDPLAYER_FLUTTER_BIN=/path/to/VoidPlayer-Flutter/bin/flutter "
        "python dev.py toolchain doctor"
    )
    sys.exit(1)


@dataclass(frozen=True)
class ToolchainCheckResult:
    ok: bool
    lock: FlutterToolchainLock
    flutter_bin: str
    flutter_root: Path | None
    version: dict[str, Any]
    lines: tuple[str, ...]


def check_flutter_toolchain() -> ToolchainCheckResult:
    lock = load_flutter_toolchain_lock()
    active_bin = flutter_bin()
    lines: list[str] = []
    version: dict[str, Any] = {}
    flutter_root: Path | None = None

    resolved_bin = shutil.which(active_bin) or active_bin
    if not Path(resolved_bin).exists() and shutil.which(active_bin) is None:
        lines.append(f"flutter executable not found: {active_bin}")
        return ToolchainCheckResult(False, lock, active_bin, None, {}, tuple(lines))

    try:
        raw_version = subprocess.check_output(
            [active_bin, "--version", "--machine"],
            cwd=str(ROOT),
            text=True,
            stderr=subprocess.STDOUT,
        )
        version = json.loads(raw_version)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        lines.append(f"cannot read flutter --version --machine: {exc}")
        return ToolchainCheckResult(False, lock, active_bin, None, version, tuple(lines))

    root_value = version.get("flutterRoot")
    if isinstance(root_value, str) and root_value:
        flutter_root = Path(root_value)
    else:
        lines.append("flutter --version did not report flutterRoot")

    expected_pairs = {
        "frameworkRevision": lock.framework_revision,
        "engineRevision": lock.engine_revision,
        "dartSdkVersion": lock.dart_sdk_version,
    }
    for key, expected in expected_pairs.items():
        actual = version.get(key)
        if actual != expected:
            lines.append(f"{key} mismatch: expected {expected}, got {actual}")

    if flutter_root is not None:
        _check_git_revision(lock, flutter_root, lines)
        _check_patch_markers(lock, flutter_root, lines)

    return ToolchainCheckResult(
        ok=not lines,
        lock=lock,
        flutter_bin=active_bin,
        flutter_root=flutter_root,
        version=version,
        lines=tuple(lines),
    )


def print_flutter_toolchain_doctor() -> None:
    result = check_flutter_toolchain()
    header("VoidPlayer Flutter Toolchain")
    print(f"Lock: {LOCK_PATH}")
    print(f"Flutter bin: {result.flutter_bin}")
    print(f"Flutter root: {result.flutter_root or '<unknown>'}")
    print(f"Fork remote: {result.lock.fork_remote}")
    print(f"Fork ref: {result.lock.fork_ref}")
    print(f"Framework revision: {result.version.get('frameworkRevision', '<unknown>')}")
    print(f"Engine revision: {result.version.get('engineRevision', '<unknown>')}")
    print(f"Dart SDK: {result.version.get('dartSdkVersion', '<unknown>')}")
    print(f"Flutter version: {result.version.get('flutterVersion', '<unknown>')}")
    if sys.platform == "darwin":
        print("\nmacOS local engines:")
        for debug, label in ((True, "Debug"), (False, "Release")):
            engine_name = local_engine_name_for_mode(debug)
            engine_path = local_engine_output_path(engine_name)
            status = "ready" if engine_path is not None and engine_path.exists() else "missing"
            print(f"  {label}: {engine_name or '<unset>'} ({status})")
            print(f"    {engine_path or '<not configured>'}")
    if result.ok:
        print("\nFlutter toolchain lock check passed.")
        return

    print("\nFlutter toolchain lock check failed:")
    for line in result.lines:
        print(f"  - {line}")
    sys.exit(1)


def bootstrap_flutter_toolchain() -> None:
    lock = load_flutter_toolchain_lock()
    target = Path(
        os.environ.get(
            "VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH",
            str(lock.default_install_path),
        )
    )
    target = target.expanduser()
    if not target.is_absolute():
        target = ROOT / target

    header("Bootstrap VoidPlayer Flutter Toolchain")
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        run(
            [
                "git",
                "clone",
                "--filter=blob:none",
                "--no-checkout",
                lock.fork_remote,
                str(target),
            ],
            cwd=str(ROOT),
        )
    elif not (target / ".git").exists():
        print(f"ERROR: target exists but is not a git checkout: {target}")
        sys.exit(1)
    else:
        _ensure_origin_remote(target, lock.fork_remote)

    run(
        ["git", "fetch", "--tags", "origin", lock.fork_ref, lock.fork_branch],
        cwd=str(target),
    )
    run(["git", "checkout", "--detach", lock.fork_commit], cwd=str(target))

    flutter = target / "bin" / _flutter_executable_name()
    env = os.environ.copy()
    env["VOIDPLAYER_FLUTTER_BIN"] = str(flutter)
    run([str(flutter), "--version"], cwd=str(ROOT), env=env)
    bootstrap_macos_local_engines(
        modes=("debug", "release"),
        flutter_bin_path=flutter,
    )
    print(f"\nFlutter toolchain ready: {target}")
    print(f"Flutter bin: {flutter}")


def print_flutter_toolchain_lock() -> None:
    print(LOCK_PATH.read_text(encoding="utf-8"))


def _check_git_revision(
    lock: FlutterToolchainLock,
    flutter_root: Path,
    lines: list[str],
) -> None:
    try:
        head = subprocess.check_output(
            ["git", "-C", str(flutter_root), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.STDOUT,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        lines.append(f"cannot read Flutter checkout git revision: {exc}")
        return

    if head != lock.fork_commit:
        lines.append(f"git HEAD mismatch: expected {lock.fork_commit}, got {head}")

    try:
        dirty = subprocess.check_output(
            ["git", "-C", str(flutter_root), "status", "--porcelain"],
            text=True,
            stderr=subprocess.STDOUT,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        lines.append(f"cannot read Flutter checkout status: {exc}")
        return

    if dirty:
        lines.append("Flutter checkout has uncommitted changes")


def _check_patch_markers(
    lock: FlutterToolchainLock,
    flutter_root: Path,
    lines: list[str],
) -> None:
    for marker in lock.required_patch_markers:
        path = flutter_root / marker["path"]
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            lines.append(f"cannot read patch marker {marker['path']}: {exc}")
            continue
        if marker["contains"] not in text:
            lines.append(
                f"patch marker missing in {marker['path']}: {marker['contains']}"
            )


def _ensure_origin_remote(target: Path, fork_remote: str) -> None:
    try:
        origin = subprocess.check_output(
            ["git", "remote", "get-url", "origin"],
            cwd=str(target),
            text=True,
            stderr=subprocess.STDOUT,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        run(["git", "remote", "add", "origin", fork_remote], cwd=str(target))
        return

    if origin != fork_remote:
        print(f"Update Flutter toolchain origin: {origin} -> {fork_remote}")
        run(["git", "remote", "set-url", "origin", fork_remote], cwd=str(target))


def _flutter_executable_name() -> str:
    return "flutter.bat" if sys.platform == "win32" else "flutter"
