#!/usr/bin/env python3
"""Initialize analysis vendor submodules without leaking checkout auth headers."""

from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ANALYSIS_SUBMODULES = (
    "native/analysis/vendor/ffmpeg",
    "native/analysis/vendor/zstd",
)
GITHUB_EXTRAHEADER = "http.https://github.com/.extraheader"


def _run(args: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(args))
    return subprocess.run(args, cwd=ROOT, check=check, text=True)


def _unset_extraheader(scope: str) -> None:
    _run(["git", "config", scope, "--unset-all", GITHUB_EXTRAHEADER], check=False)


def main() -> None:
    _unset_extraheader("--local")
    _unset_extraheader("--global")
    git_no_header = ["git", "-c", f"{GITHUB_EXTRAHEADER}="]
    _run([*git_no_header, "submodule", "sync", "--", *ANALYSIS_SUBMODULES])
    _run([
        *git_no_header,
        "submodule",
        "update",
        "--init",
        "--recursive",
        "--checkout",
        *ANALYSIS_SUBMODULES,
    ])


if __name__ == "__main__":
    main()
