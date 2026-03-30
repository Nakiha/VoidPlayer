"""Root build script — orchestrates native module build/test and Flutter build."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
NATIVE_BUILD = ROOT / "windows" / "native" / "build.py"


def run(cmd, **kwargs):
    print(f"\n{'='*60}")
    print(f"> {' '.join(str(c) for c in cmd)}")
    print('='*60)
    subprocess.check_call(cmd, shell=True, **kwargs)


def run_native(args, debug):
    """Build and test native module. Returns True if all tests pass."""
    # Step 1a: Build (always required)
    build_cmd = [sys.executable, str(NATIVE_BUILD), "--build-only"]
    if debug:
        build_cmd.append("--debug")
    run(build_cmd, cwd=str(ROOT / "windows" / "native"))

    # Step 1b: Test (non-blocking)
    if not args.no_test:
        test_cmd = [sys.executable, str(NATIVE_BUILD), "--test-only"]
        if debug:
            test_cmd.append("--debug")
        try:
            run(test_cmd, cwd=str(ROOT / "windows" / "native"))
            return True
        except subprocess.CalledProcessError:
            print("\nWARNING: Native tests failed (continuing with Flutter build)")
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description="Build VoidPlayer")
    parser.add_argument("--debug", action="store_true", help="Debug mode")
    parser.add_argument("--native-only", action="store_true",
                        help="Only build/test native module, skip Flutter")
    parser.add_argument("--flutter-only", action="store_true",
                        help="Only build Flutter, skip native module")
    parser.add_argument("--no-test", action="store_true",
                        help="Skip native tests")
    args = parser.parse_args()

    if args.native_only and args.flutter_only:
        parser.error("--native-only and --flutter-only are mutually exclusive")

    # Step 1: Build + test native module
    if not args.flutter_only:
        run_native(args, args.debug)

    # Step 2: Flutter build
    if not args.native_only:
        flutter_cmd = ["flutter", "build", "windows"]
        if args.debug:
            flutter_cmd.append("--debug")
        run(flutter_cmd, cwd=str(ROOT))

    print("\nAll done.")


if __name__ == "__main__":
    main()
