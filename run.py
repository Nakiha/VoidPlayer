"""Run the VoidPlayer Flutter application."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent


def main():
    subprocess.check_call(["flutter", "run", "-d", "windows"], cwd=str(ROOT), shell=True)


if __name__ == "__main__":
    main()
