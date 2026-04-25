"""Process helpers for development commands."""

import shutil
import subprocess
from pathlib import Path


def header(title: str) -> None:
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print("=" * 60)


def run(cmd, **kwargs) -> None:
    """Run a command and handle Windows .bat/.cmd launchers."""
    cmd = [str(part) for part in cmd]
    print(f"> {subprocess.list2cmdline(cmd)}")

    executable = shutil.which(cmd[0])
    if executable:
        cmd[0] = executable
        if Path(executable).suffix.lower() in {".bat", ".cmd"}:
            cmd = ["cmd.exe", "/c", *cmd]

    subprocess.check_call(cmd, **kwargs)
