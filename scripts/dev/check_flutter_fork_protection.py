"""Static guardrails for the pinned VoidPlayer Flutter fork."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


LOCK_PATH = ROOT / "toolchains" / "flutter.lock.json"
PATCH_DOC_PATH = ROOT / "toolchains" / "FLUTTER_FORK_PATCHES.md"

REQUIRED_LOCK_FIELDS = [
    "schemaVersion",
    "name",
    "upstreamFlutter",
    "flutterVersion",
    "dartSdkVersion",
    "frameworkRevision",
    "engineRevision",
    "forkRemote",
    "forkRef",
    "forkBranch",
    "forkCommit",
    "defaultInstallPath",
    "macosLocalEngineReleaseTag",
    "macosLocalEngineArtifacts",
    "windowsLocalEngineArtifacts",
    "requiredPatchMarkers",
    "platformProfiles",
]

REQUIRED_PATCH_MARKERS = {
    "engine/src/flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h": (
        "voidPlayerHDRCurrentFlutterSurfaceInfos"
    ),
    "engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine.mm": (
        "VoidPlayerHDRSerializableSurfaceInfo"
    ),
    "engine/src/flutter/shell/platform/windows/public/flutter_windows.h": (
        "FLUTTER_WINDOWS_SURFACE_EXPORT_API"
    ),
    "engine/src/flutter/shell/platform/windows/flutter_windows_surface_export.cc": (
        "kFlutterDesktopWindowsSurfaceBackendD3D12"
    ),
}

REQUIRED_PLATFORM_PATCH_MARKERS = {
    "macos": {
        "engine/src/flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h": (
            "voidPlayerHDRCurrentFlutterSurfaceInfos"
        ),
        "engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine.mm": (
            "VoidPlayerHDRSerializableSurfaceInfo"
        ),
    },
    "windows": {
        "engine/src/flutter/shell/platform/windows/public/flutter_windows.h": (
            "FLUTTER_WINDOWS_SURFACE_EXPORT_API"
        ),
        "engine/src/flutter/shell/platform/windows/flutter_windows_surface_export.cc": (
            "kFlutterDesktopWindowsSurfaceBackendD3D12"
        ),
    },
}

REQUIRED_WORKFLOW_SNIPPETS = {
    ".github/workflows/flutter.yml": [
        "hashFiles('toolchains/flutter.lock.json')",
        "scripts/ci/bootstrap_flutter_toolchain.sh",
    ],
    ".github/workflows/native.yml": [
        "hashFiles('toolchains/flutter.lock.json')",
        "scripts/ci/bootstrap_flutter_toolchain.ps1",
        "scripts/ci/bootstrap_flutter_toolchain.sh",
        "scripts/ci/bootstrap_flutter_macos_engine.sh",
    ],
    ".github/workflows/macos-ui.yml": [
        "hashFiles('toolchains/flutter.lock.json')",
        "scripts/ci/bootstrap_flutter_toolchain.sh",
        "scripts/ci/bootstrap_flutter_macos_engine.sh",
    ],
}

REQUIRED_TOOLCHAIN_DOC_SNIPPETS = [
    "python dev.py toolchain bootstrap-flutter",
    "python dev.py toolchain doctor",
    "Do not move an existing `voidplayer-flutter-*-hdr.*` tag",
]

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def _load_lock(errors: list[str]) -> dict[str, Any]:
    try:
        raw = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"cannot read {LOCK_PATH.relative_to(ROOT)}: {exc}")
        return {}
    if not isinstance(raw, dict):
        errors.append(f"{LOCK_PATH.relative_to(ROOT)} root value must be an object")
        return {}
    return raw


def _string_field(
    lock: dict[str, Any],
    key: str,
    errors: list[str],
) -> str:
    value = lock.get(key)
    if not isinstance(value, str) or not value:
        errors.append(f"toolchains/flutter.lock.json is missing string field: {key}")
        return ""
    return value


def _check_lock_shape(lock: dict[str, Any], errors: list[str]) -> None:
    for field in REQUIRED_LOCK_FIELDS:
        if field not in lock:
            errors.append(f"toolchains/flutter.lock.json is missing field: {field}")

    if lock.get("schemaVersion") != 1:
        errors.append("toolchains/flutter.lock.json schemaVersion must be 1")

    fork_remote = _string_field(lock, "forkRemote", errors)
    if fork_remote and not (
        fork_remote.startswith("https://github.com/") and fork_remote.endswith(".git")
    ):
        errors.append("toolchains/flutter.lock.json forkRemote must be a GitHub HTTPS .git URL")

    framework_revision = _string_field(lock, "frameworkRevision", errors)
    fork_commit = _string_field(lock, "forkCommit", errors)
    engine_revision = _string_field(lock, "engineRevision", errors)
    for key, value in (
        ("frameworkRevision", framework_revision),
        ("forkCommit", fork_commit),
        ("engineRevision", engine_revision),
    ):
        if value and not GIT_SHA_RE.match(value):
            errors.append(f"toolchains/flutter.lock.json {key} must be a full git SHA")
    if framework_revision and fork_commit and framework_revision != fork_commit:
        errors.append("toolchains/flutter.lock.json frameworkRevision must match forkCommit")

    _check_engine_artifacts(lock, "macosLocalEngineArtifacts", "macos", errors)
    _check_engine_artifacts(lock, "windowsLocalEngineArtifacts", "windows", errors)
    _check_patch_markers(lock, errors)
    _check_platform_profiles(lock, errors)


def _check_engine_artifacts(
    lock: dict[str, Any],
    key: str,
    platform_token: str,
    errors: list[str],
) -> None:
    artifacts = lock.get(key)
    if not isinstance(artifacts, dict):
        errors.append(f"toolchains/flutter.lock.json {key} must be an object")
        return
    for mode in ("debug", "release"):
        spec = artifacts.get(mode)
        if not isinstance(spec, dict):
            errors.append(f"toolchains/flutter.lock.json {key}.{mode} must be an object")
            continue
        for field in ("engine", "host", "asset", "sha256"):
            value = spec.get(field)
            if not isinstance(value, str) or not value:
                errors.append(f"toolchains/flutter.lock.json {key}.{mode}.{field} is missing")
        asset = spec.get("asset")
        if isinstance(asset, str) and platform_token not in asset:
            errors.append(f"toolchains/flutter.lock.json {key}.{mode}.asset should name {platform_token}")
        sha256 = spec.get("sha256")
        if isinstance(sha256, str) and not SHA256_RE.match(sha256.lower()):
            errors.append(f"toolchains/flutter.lock.json {key}.{mode}.sha256 must be SHA-256")

    if key == "macosLocalEngineArtifacts":
        release_tag = _string_field(lock, "macosLocalEngineReleaseTag", errors)
        if release_tag:
            for mode in ("debug", "release"):
                spec = artifacts.get(mode)
                asset = spec.get("asset") if isinstance(spec, dict) else None
                if isinstance(asset, str) and release_tag not in asset:
                    errors.append(
                        f"toolchains/flutter.lock.json {key}.{mode}.asset must include "
                        f"macosLocalEngineReleaseTag"
                    )

    if key == "windowsLocalEngineArtifacts":
        fork_commit = _string_field(lock, "forkCommit", errors)
        prefix = fork_commit[:12] if fork_commit else ""
        for mode in ("debug", "release"):
            spec = artifacts.get(mode)
            asset = spec.get("asset") if isinstance(spec, dict) else None
            if prefix and isinstance(asset, str) and prefix not in asset:
                errors.append(
                    f"toolchains/flutter.lock.json {key}.{mode}.asset must include "
                    f"the fork commit prefix"
                )


def _check_patch_markers(lock: dict[str, Any], errors: list[str]) -> None:
    markers = lock.get("requiredPatchMarkers")
    if not isinstance(markers, list):
        errors.append("toolchains/flutter.lock.json requiredPatchMarkers must be a list")
        return
    marker_map: dict[str, str] = {}
    for item in markers:
        if not isinstance(item, dict):
            errors.append("toolchains/flutter.lock.json requiredPatchMarkers entries must be objects")
            continue
        path = item.get("path")
        contains = item.get("contains")
        if not isinstance(path, str) or not isinstance(contains, str):
            errors.append("toolchains/flutter.lock.json patch marker path/contains must be strings")
            continue
        marker_map[path] = contains
    for path, contains in REQUIRED_PATCH_MARKERS.items():
        if marker_map.get(path) != contains:
            errors.append(f"toolchains/flutter.lock.json is missing required patch marker: {path}")


def _check_platform_profiles(lock: dict[str, Any], errors: list[str]) -> None:
    profiles = lock.get("platformProfiles")
    if not isinstance(profiles, dict):
        errors.append("toolchains/flutter.lock.json platformProfiles must be an object")
        return
    for platform, required_markers in REQUIRED_PLATFORM_PATCH_MARKERS.items():
        profile = profiles.get(platform)
        if not isinstance(profile, dict):
            errors.append(
                f"toolchains/flutter.lock.json platformProfiles.{platform} must be an object"
            )
            continue
        for field in (
            "flutterVersion",
            "frameworkRevision",
            "forkRef",
            "forkBranch",
            "forkCommit",
        ):
            value = profile.get(field)
            if not isinstance(value, str) or not value:
                errors.append(
                    f"toolchains/flutter.lock.json platformProfiles.{platform}.{field} is missing"
                )
        revision = profile.get("frameworkRevision")
        commit = profile.get("forkCommit")
        for field, value in (
            ("frameworkRevision", revision),
            ("forkCommit", commit),
        ):
            if isinstance(value, str) and value and not GIT_SHA_RE.match(value):
                errors.append(
                    f"toolchains/flutter.lock.json platformProfiles.{platform}.{field} must be a full git SHA"
                )
        if isinstance(revision, str) and isinstance(commit, str) and revision != commit:
            errors.append(
                f"toolchains/flutter.lock.json platformProfiles.{platform}.frameworkRevision must match forkCommit"
            )
        markers = profile.get("requiredPatchMarkers")
        if not isinstance(markers, list):
            errors.append(
                f"toolchains/flutter.lock.json platformProfiles.{platform}.requiredPatchMarkers must be a list"
            )
            continue
        marker_map: dict[str, str] = {}
        for item in markers:
            if not isinstance(item, dict):
                errors.append(
                    f"toolchains/flutter.lock.json platformProfiles.{platform}.requiredPatchMarkers entries must be objects"
                )
                continue
            path = item.get("path")
            contains = item.get("contains")
            if not isinstance(path, str) or not isinstance(contains, str):
                errors.append(
                    f"toolchains/flutter.lock.json platformProfiles.{platform} patch marker path/contains must be strings"
                )
                continue
            marker_map[path] = contains
        for path, contains in required_markers.items():
            if marker_map.get(path) != contains:
                errors.append(
                    f"toolchains/flutter.lock.json platformProfiles.{platform} is missing patch marker: {path}"
                )


def _check_patch_doc(lock: dict[str, Any], errors: list[str]) -> None:
    try:
        text = PATCH_DOC_PATH.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"cannot read {PATCH_DOC_PATH.relative_to(ROOT)}: {exc}")
        return

    required_values = [
        _string_field(lock, "forkRemote", errors),
        _string_field(lock, "forkRef", errors),
        _string_field(lock, "forkBranch", errors),
        _string_field(lock, "forkCommit", errors),
        _string_field(lock, "frameworkRevision", errors),
        _string_field(lock, "engineRevision", errors),
        _string_field(lock, "dartSdkVersion", errors),
        _string_field(lock, "macosLocalEngineReleaseTag", errors),
    ]
    for value in {item for item in required_values if item}:
        if value not in text:
            errors.append(f"toolchains/FLUTTER_FORK_PATCHES.md is missing lock value: {value}")

    for snippet in REQUIRED_TOOLCHAIN_DOC_SNIPPETS:
        if snippet not in text:
            errors.append(f"toolchains/FLUTTER_FORK_PATCHES.md is missing: {snippet}")


def _check_workflows(errors: list[str]) -> None:
    for rel, snippets in REQUIRED_WORKFLOW_SNIPPETS.items():
        path = ROOT / rel
        if not path.is_file():
            errors.append(f"missing workflow for Flutter fork protection: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        for snippet in snippets:
            if snippet not in text:
                errors.append(f"{rel} is missing Flutter fork protection snippet: {snippet}")


def check_flutter_fork_protection() -> list[str]:
    errors: list[str] = []
    lock = _load_lock(errors)
    if lock:
        _check_lock_shape(lock, errors)
        _check_patch_doc(lock, errors)
    _check_workflows(errors)
    return errors


def main() -> int:
    errors = check_flutter_fork_protection()
    if errors:
        print("Flutter fork protection check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("Flutter fork protection check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
