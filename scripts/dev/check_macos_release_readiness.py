"""Check macOS package release-readiness evidence.

This is a packaging smoke, not a legal review or notarization substitute. It
keeps macOS release inputs visible: FFmpeg dylibs, rpaths, license notices,
codesign entitlements, sandbox file access, and crash/log staging boundaries.
"""

from __future__ import annotations

import argparse
import fnmatch
import plistlib
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.dev.check_release_compliance import check_source_tree, check_stage  # noqa: E402
from scripts.dev.macos_ffmpeg import (  # noqa: E402
    FFMPEG_RUNTIME_LIBRARIES,
    ffmpeg_runtime_dylibs,
    ffmpeg_runtime_symlinks,
)
from scripts.dev.paths import MACOS_PACKAGE_STAGE_DIR  # noqa: E402


MUTABLE_DIR_NAMES = {
    ".pytest_cache",
    "__pycache__",
    "cache",
    "caches",
    "crash",
    "crashes",
    "log",
    "logs",
    "temp",
    "tmp",
    "userdata",
    "user_data",
}

MUTABLE_FILE_NAMES = {
    "analysis_index.json",
    "config.json",
}

MUTABLE_FILE_PATTERNS = {
    "*.dmp",
    "*.ips",
    "*.log",
    "*.tmp",
    "*.vac",
    "*.vck",
}


def _run_capture(cmd: list[str]) -> subprocess.CompletedProcess:
    print("> " + " ".join(cmd))
    return subprocess.run(
        cmd,
        cwd=str(ROOT),
        check=True,
        capture_output=True,
    )


def _require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"missing {label}: {path}")


def _require_dir(path: Path, label: str) -> None:
    if not path.is_dir():
        raise RuntimeError(f"missing {label}: {path}")


def _require_text(path: Path, needles: list[str], label: str) -> None:
    _require_file(path, label)
    text = path.read_text(encoding="utf-8", errors="ignore")
    missing = [needle for needle in needles if needle not in text]
    if missing:
        joined = ", ".join(repr(value) for value in missing)
        raise RuntimeError(f"{label} is missing expected text: {joined}")


def _load_plist(path: Path) -> dict:
    _require_file(path, "plist")
    with path.open("rb") as src:
        return plistlib.load(src)


def _check_source_inputs() -> None:
    check_source_tree()

    release_entitlements = _load_plist(ROOT / "macos" / "Runner" / "Release.entitlements")
    _require_entitlement(release_entitlements, "com.apple.security.app-sandbox", True)
    _require_entitlement(release_entitlements, "com.apple.security.files.user-selected.read-only", True)
    if release_entitlements.get("com.apple.security.cs.allow-jit") is True:
        raise RuntimeError("Release.entitlements must not enable JIT")
    if release_entitlements.get("com.apple.security.network.server") is True:
        raise RuntimeError("Release.entitlements must not enable network.server")

    _require_text(
        ROOT / "macos" / "scripts" / "embed_ffmpeg_dylibs.sh",
        [*FFMPEG_RUNTIME_LIBRARIES, "ThirdParty/ffmpeg", "codesign"],
        "macOS FFmpeg embed script",
    )
    _require_text(
        ROOT / "macos" / "Runner" / "MacOSFilePicker.swift",
        [
            "NSOpenPanel",
            "canChooseFiles = true",
            "panel.urls.map",
            "startAccessingSecurityScopedResource",
            "activateSecurityScopedBookmarks",
        ],
        "macOS sandbox file picker",
    )
    _require_text(
        ROOT / "scripts" / "dev" / "flutter_app.py",
        ["DiagnosticReports", "_wait_for_new_macos_crash_reports"],
        "macOS crash report watcher",
    )


def _require_entitlement(entitlements: dict, key: str, expected: bool) -> None:
    actual = entitlements.get(key)
    if actual is not expected:
        raise RuntimeError(f"expected entitlement {key}={expected}, got {actual!r}")


def _find_mutable_artifacts(root: Path) -> list[Path]:
    offenders: list[Path] = []
    for path in root.rglob("*"):
        name = path.name.lower()
        if path.is_dir() and name in MUTABLE_DIR_NAMES:
            offenders.append(path)
            continue
        if path.is_file():
            if name in MUTABLE_FILE_NAMES:
                offenders.append(path)
                continue
            if any(fnmatch.fnmatch(name, pattern) for pattern in MUTABLE_FILE_PATTERNS):
                offenders.append(path)
    return offenders


def _check_no_mutable_artifacts(stage_dir: Path) -> None:
    offenders = _find_mutable_artifacts(stage_dir)
    if not offenders:
        return

    lines = [f"  - {path.relative_to(stage_dir)}" for path in offenders[:30]]
    if len(offenders) > 30:
        lines.append(f"  ... and {len(offenders) - 30} more")
    raise RuntimeError("macOS package staging contains runtime/user artifacts:\n" + "\n".join(lines))


def _otool_libraries(binary: Path) -> list[str]:
    result = _run_capture(["otool", "-L", str(binary)])
    text = result.stdout.decode("utf-8", errors="ignore")
    libraries: list[str] = []
    for line in text.splitlines()[1:]:
        stripped = line.strip()
        if stripped:
            libraries.append(stripped.split(" (", 1)[0])
    return libraries


def _check_macos_linkage(stage_app: Path) -> None:
    executable = stage_app / "Contents" / "MacOS" / "VoidPlayer"
    frameworks = stage_app / "Contents" / "Frameworks"
    ffmpeg_lib_dir = ROOT / ".toolchains" / "ffmpeg" / "macos-arm64" / "lib"
    ffmpeg_dylibs = ffmpeg_runtime_dylibs(ffmpeg_lib_dir)
    ffmpeg_symlinks = ffmpeg_runtime_symlinks(ffmpeg_lib_dir)
    required_loads = {f"@rpath/{name}" for name in ffmpeg_dylibs}

    _require_file(executable, "macOS executable")
    _require_dir(frameworks, "macOS Frameworks directory")

    for name in ffmpeg_dylibs:
        _require_file(frameworks / name, f"bundled FFmpeg dylib {name}")
    for name in ffmpeg_symlinks:
        path = frameworks / name
        if not path.is_symlink():
            raise RuntimeError(f"missing bundled FFmpeg dylib symlink: {path}")

    executable_loads = set(_otool_libraries(executable))
    missing = sorted(required_loads - executable_loads)
    if missing:
        raise RuntimeError("staged app executable is missing FFmpeg @rpath loads: " + ", ".join(missing))

    _check_no_developer_paths(executable, executable_loads)
    for name in ffmpeg_dylibs:
        dylib = frameworks / name
        loads = _otool_libraries(dylib)
        if not loads or loads[0] != f"@rpath/{name}":
            actual = loads[0] if loads else "<none>"
            raise RuntimeError(f"{dylib.name} has unexpected install name: {actual}")
        for library in loads[1:]:
            dep_name = Path(library).name
            if dep_name.startswith(("libav", "libswresample")) and not library.startswith("@rpath/"):
                raise RuntimeError(f"{dylib.name} has non-rpath FFmpeg dependency: {library}")
        _check_no_developer_paths(dylib, loads)


def _check_no_developer_paths(binary: Path, loads: set[str] | list[str]) -> None:
    forbidden = (str(ROOT), "/.toolchains/ffmpeg/", "/native/build", "/build/macos/")
    for library in loads:
        if any(marker in library for marker in forbidden):
            raise RuntimeError(f"{binary.name} has developer-machine linkage path: {library}")


def _codesign_entitlements(stage_app: Path) -> dict:
    result = _run_capture(["codesign", "-d", "--entitlements", ":-", str(stage_app)])
    for data in (result.stdout, result.stderr):
        xml_start = data.find(b"<?xml")
        if xml_start >= 0:
            return plistlib.loads(data[xml_start:])
        binary_start = data.find(b"bplist")
        if binary_start >= 0:
            return plistlib.loads(data[binary_start:])
    raise RuntimeError("staged app signature does not expose entitlements")


def _check_codesign(stage_app: Path, require_developer_id: bool) -> None:
    _run_capture(["codesign", "--verify", "--deep", "--strict", str(stage_app)])
    result = _run_capture(["codesign", "-dv", "--verbose=4", str(stage_app)])
    display = (result.stdout + result.stderr).decode("utf-8", errors="ignore")
    if require_developer_id and "Authority=Developer ID Application:" not in display:
        raise RuntimeError("staged app is not signed with a Developer ID Application identity")
    if require_developer_id:
        _run_capture(["spctl", "-a", "-vv", "--type", "execute", str(stage_app)])

    entitlements = _codesign_entitlements(stage_app)
    _require_entitlement(entitlements, "com.apple.security.app-sandbox", True)
    _require_entitlement(entitlements, "com.apple.security.files.user-selected.read-only", True)


def _check_app_compliance(stage_app: Path) -> None:
    resources = stage_app / "Contents" / "Resources"
    ffmpeg_docs = resources / "ThirdParty" / "ffmpeg"
    for path, label in [
        (resources / "docs" / "LICENSE", "app GPL license"),
        (resources / "docs" / "THIRD_PARTY_NOTICES.md", "app third-party notices"),
        (resources / "docs" / "THIRD_PARTY_NATIVE.md", "app native third-party manifest"),
        (ffmpeg_docs / "README.txt", "app FFmpeg README"),
        (ffmpeg_docs / "VOIDPLAYER_BUILD.md", "app FFmpeg build notes"),
        (ffmpeg_docs / "voidplayer-ffmpeg-manifest.json", "app FFmpeg manifest"),
        (ffmpeg_docs / "LICENSES" / "FFmpeg-LICENSE.md", "app FFmpeg license"),
    ]:
        _require_file(path, label)


def _check_nested_code_layout(stage_app: Path) -> None:
    helpers_analyzer = (
        stage_app
        / "Contents"
        / "Helpers"
        / "ffmpeg-analysis"
        / "void_ffmpeg_analyzer"
    )
    _require_file(helpers_analyzer, "macOS FFmpeg analyzer helper")
    if not helpers_analyzer.stat().st_mode & 0o111:
        raise RuntimeError(f"macOS FFmpeg analyzer helper is not executable: {helpers_analyzer}")

    legacy_tools_dir = stage_app / "Contents" / "MacOS" / "tools"
    if legacy_tools_dir.exists():
        raise RuntimeError(
            "legacy macOS helper location is present under Contents/MacOS/tools; "
            "place helper executables under Contents/Helpers"
        )


def _check_stage(stage_dir: Path, require_developer_id: bool) -> None:
    _require_dir(stage_dir, "macOS package stage")
    stage_app = stage_dir / "VoidPlayer.app"
    _require_dir(stage_app, "staged VoidPlayer.app")
    _require_file(stage_app / "Contents" / "Info.plist", "staged Info.plist")

    check_stage(stage_dir)
    _check_app_compliance(stage_app)
    _check_nested_code_layout(stage_app)
    _check_no_mutable_artifacts(stage_dir)
    _check_macos_linkage(stage_app)
    _check_codesign(stage_app, require_developer_id)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, default=MACOS_PACKAGE_STAGE_DIR,
                        help="macOS package stage directory")
    parser.add_argument("--require-developer-id", action="store_true",
                        help="Require a Developer ID Application signature")
    args = parser.parse_args()

    try:
        _check_source_inputs()
        _check_stage(args.stage, args.require_developer_id)
    except (RuntimeError, subprocess.CalledProcessError, plistlib.InvalidFileException) as exc:
        print(f"ERROR: macOS release readiness failed: {exc}")
        return 1

    print("macOS release readiness smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
