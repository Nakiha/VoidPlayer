"""Lightweight repository consistency checks."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from .check_flutter_fork_protection import check_flutter_fork_protection
from .check_macos_platform_protection import check_macos_platform_protection
from .check_windows_fork_protection import check_windows_fork_protection
from .paths import ROOT


GENERATED_WINDOWS_PLUGIN_FILES = [
    "windows/flutter/generated_plugin_registrant.cc",
    "windows/flutter/generated_plugin_registrant.h",
    "windows/flutter/generated_plugins.cmake",
]

REMOVED_RUNTIME_FFMPEG_ROOTS = [
    "windows/libs/ffmpeg",
    "third_party/ffmpeg",
]

REQUIRED_PATHS = [
    "lib/main_window",
    "toolchains/ffmpeg.lock.json",
    ".toolchains/ffmpeg",
]

DEPRECATED_GITHUB_ACTIONS = {
    "actions/checkout@v4": "actions/checkout@v6",
    "actions/setup-python@v5": "actions/setup-python@v6",
    "actions/cache@v4": "actions/cache@v5",
    "actions/upload-artifact@v4": "actions/upload-artifact@v7",
    "actions/download-artifact@v4": "actions/download-artifact@v8",
}


def _git_ls_files(*paths: str) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", *paths],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def _collect_markdown_links(path: Path) -> list[tuple[int, str]]:
    links: list[tuple[int, str]] = []
    pattern = re.compile(r"\[[^\]]+\]\(([^)]+)\)")
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        for match in pattern.finditer(line):
            target = match.group(1).split("#", 1)[0]
            if not target or "://" in target or target.startswith(("mailto:", "#")):
                continue
            links.append((lineno, target))
    return links


def _check_markdown_links(errors: list[str]) -> None:
    for path in sorted(ROOT.rglob("*.md")):
        relative = path.relative_to(ROOT)
        if relative.parts and relative.parts[0] in {
            ".dart_tool",
            ".git",
            ".toolchains",
            "build",
        }:
            continue
        if (
            relative.parts[:3] == ("native", "analysis", "vendor")
            or relative.parts[:2] == ("third_party", "ffmpeg")
            or relative.parts[:3] == ("windows", "libs", "ffmpeg")
        ):
            continue
        if any(part in {"native"} for part in relative.parts):
            if "native/docs" not in path.as_posix() and path.name not in {"README.md", "AGENTS.md"}:
                continue
        for lineno, target in _collect_markdown_links(path):
            if target.startswith("/"):
                candidate = ROOT / target.lstrip("/")
            else:
                candidate = (path.parent / target).resolve()
            if not candidate.exists():
                rel = path.relative_to(ROOT)
                errors.append(f"{rel}:{lineno}: broken markdown link: {target}")


def _check_tracked_generated(errors: list[str]) -> None:
    tracked = set(_git_ls_files(*GENERATED_WINDOWS_PLUGIN_FILES))
    for path in GENERATED_WINDOWS_PLUGIN_FILES:
        if path in tracked:
            errors.append(f"{path} is generated but still tracked")


def _check_runtime_ffmpeg_tracking(errors: list[str]) -> None:
    tracked = _git_ls_files(*REMOVED_RUNTIME_FFMPEG_ROOTS)
    if tracked:
        shown = ", ".join(tracked[:5])
        suffix = "" if len(tracked) <= 5 else f", ... ({len(tracked)} files)"
        errors.append(f"runtime FFmpeg SDK files are still tracked: {shown}{suffix}")


def _check_required_paths(errors: list[str]) -> None:
    for rel in REQUIRED_PATHS:
        if rel == ".toolchains/ffmpeg":
            continue
        if not (ROOT / rel).exists():
            errors.append(f"required path is missing: {rel}")


def _check_github_actions_versions(errors: list[str]) -> None:
    workflow_dir = ROOT / ".github" / "workflows"
    for path in sorted(workflow_dir.glob("*.yml")):
        text = path.read_text(encoding="utf-8")
        for deprecated, replacement in DEPRECATED_GITHUB_ACTIONS.items():
            if deprecated in text:
                rel = path.relative_to(ROOT)
                errors.append(f"{rel} uses {deprecated}; use {replacement}")


def cmd_repo_hygiene(args: argparse.Namespace) -> None:
    errors: list[str] = []
    _check_required_paths(errors)
    _check_tracked_generated(errors)
    _check_runtime_ffmpeg_tracking(errors)
    _check_markdown_links(errors)
    _check_github_actions_versions(errors)
    errors.extend(check_flutter_fork_protection())
    errors.extend(check_macos_platform_protection())
    errors.extend(check_windows_fork_protection())
    if errors:
        print("Repository hygiene check failed:")
        for error in errors:
            print(f"  - {error}")
        sys.exit(1)
    print("Repository hygiene check passed.")
