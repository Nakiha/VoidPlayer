#!/usr/bin/env python3
"""Read and validate the pinned VoidPlayer FFmpeg artifact lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
LOCK_PATH = ROOT / "toolchains" / "ffmpeg.lock.json"


def _load_lock() -> dict[str, Any]:
    try:
        with LOCK_PATH.open("r", encoding="utf-8") as fh:
            lock = json.load(fh)
    except FileNotFoundError:
        print(f"ERROR: missing FFmpeg lock: {LOCK_PATH}", file=sys.stderr)
        sys.exit(2)
    if lock.get("schemaVersion") != 1:
        print("ERROR: unsupported FFmpeg lock schema", file=sys.stderr)
        sys.exit(2)
    return lock


def _artifact(lock: dict[str, Any], platform: str) -> dict[str, Any]:
    artifacts = lock.get("artifacts", {})
    artifact = artifacts.get(platform)
    if not isinstance(artifact, dict):
        print(f"ERROR: FFmpeg lock has no artifact for {platform}", file=sys.stderr)
        sys.exit(2)
    return artifact


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _cmd_run_id(args: argparse.Namespace) -> None:
    lock = _load_lock()
    print(lock["workflowRunId"])


def _cmd_repository(args: argparse.Namespace) -> None:
    lock = _load_lock()
    print(lock["repository"])


def _cmd_artifact_name(args: argparse.Namespace) -> None:
    lock = _load_lock()
    print(_artifact(lock, args.platform)["name"])


def _cmd_artifact_id(args: argparse.Namespace) -> None:
    lock = _load_lock()
    print(_artifact(lock, args.platform)["githubArtifactId"])


def _cmd_install_path(args: argparse.Namespace) -> None:
    lock = _load_lock()
    install_root = lock.get("defaultInstallPath", ".toolchains/ffmpeg")
    print(str(ROOT / install_root / args.platform))


def _cmd_verify(args: argparse.Namespace) -> None:
    lock = _load_lock()
    artifact = _artifact(lock, args.platform)
    zip_path = Path(args.zip_path)
    expected = artifact.get("zipSha256")
    if not expected:
        print(f"No inner zip checksum pinned for {args.platform}; skipped inner zip checksum.")
        return
    actual = _sha256(zip_path)
    if actual.lower() != expected.lower():
        print(
            f"ERROR: FFmpeg artifact checksum mismatch for {args.platform}\n"
            f"  file:     {zip_path}\n"
            f"  expected: {expected}\n"
            f"  actual:   {actual}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"Verified {args.platform} FFmpeg artifact: {actual}")


def _cmd_verify_github_artifact(args: argparse.Namespace) -> None:
    lock = _load_lock()
    artifact = _artifact(lock, args.platform)
    archive_path = Path(args.archive_path)
    expected = artifact.get("githubArtifactSha256")
    if not expected:
        print(
            f"ERROR: FFmpeg lock artifact {args.platform} has no githubArtifactSha256",
            file=sys.stderr,
        )
        sys.exit(2)
    actual = _sha256(archive_path)
    if actual.lower() != expected.lower():
        print(
            f"ERROR: GitHub FFmpeg artifact checksum mismatch for {args.platform}\n"
            f"  file:     {archive_path}\n"
            f"  expected: {expected}\n"
            f"  actual:   {actual}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"Verified {args.platform} GitHub artifact archive: {actual}")


def _cmd_build_notes(args: argparse.Namespace) -> None:
    lock = _load_lock()
    artifact = _artifact(lock, args.platform)
    print("# VoidPlayer FFmpeg Package")
    print()
    print("Restored from the pinned VoidPlayer FFmpeg lock.")
    print()
    print(f"- Repository: https://github.com/{lock['repository']}")
    print(f"- Workflow: {lock.get('workflow', 'Build FFmpeg')}")
    print(f"- GitHub Actions run: https://github.com/{lock['repository']}/actions/runs/{lock['workflowRunId']}")
    print(f"- Source commit: {lock['sourceCommit']}")
    print(f"- FFmpeg version: {lock['ffmpegVersion']}")
    print(f"- Artifact: {artifact['name']}")
    print(f"- GitHub artifact id: {artifact['githubArtifactId']}")
    print(f"- GitHub artifact SHA-256: {artifact['githubArtifactSha256']}")
    if artifact.get("zipSha256"):
        print(f"- Inner package SHA-256: {artifact['zipSha256']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_run_id = sub.add_parser("run-id")
    p_run_id.set_defaults(func=_cmd_run_id)

    p_repository = sub.add_parser("repository")
    p_repository.set_defaults(func=_cmd_repository)

    p_artifact = sub.add_parser("artifact-name")
    p_artifact.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_artifact.set_defaults(func=_cmd_artifact_name)

    p_artifact_id = sub.add_parser("artifact-id")
    p_artifact_id.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_artifact_id.set_defaults(func=_cmd_artifact_id)

    p_install = sub.add_parser("install-path")
    p_install.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_install.set_defaults(func=_cmd_install_path)

    p_verify = sub.add_parser("verify")
    p_verify.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_verify.add_argument("zip_path")
    p_verify.set_defaults(func=_cmd_verify)

    p_verify_github = sub.add_parser("verify-github-artifact")
    p_verify_github.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_verify_github.add_argument("archive_path")
    p_verify_github.set_defaults(func=_cmd_verify_github_artifact)

    p_notes = sub.add_parser("build-notes")
    p_notes.add_argument("platform", choices=["windows-x64", "macos-arm64"])
    p_notes.set_defaults(func=_cmd_build_notes)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
