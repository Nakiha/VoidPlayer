"""Native standalone build and test commands."""

import sys

from .paths import NATIVE_BUILD_PY, NATIVE_DIR
from .process import header, run


def native_build(debug: bool, test: bool = True) -> None:
    """Build native standalone module, optionally run tests."""
    build_type = "Debug" if debug else "Release"

    header(f"Build native standalone ({build_type})")
    build_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--build-only"]
    if debug:
        build_cmd.append("--debug")
    run(build_cmd, cwd=str(NATIVE_DIR))

    if test:
        header(f"Test native standalone ({build_type})")
        test_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--test-only"]
        if debug:
            test_cmd.append("--debug")
        run(test_cmd, cwd=str(NATIVE_DIR))
