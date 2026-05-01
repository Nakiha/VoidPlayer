"""Native standalone build and test commands."""

import sys

from .paths import NATIVE_BUILD_PY, NATIVE_DIR, find_vtm_decoder
from .process import header, run


def ensure_analysis_test_tools() -> None:
    """Prepare external tools required by native analysis tests."""
    decoder = find_vtm_decoder()
    if decoder.exists():
        return

    header("Prepare VTM DecoderApp for analysis tests")
    print("VTM DecoderApp missing; building it before native tests...")
    print("Tip: set VTM_DECODER_APP=C:\\path\\to\\DecoderApp.exe to reuse a prebuilt VTM.")

    from .vtm import cmd_vtm_build, ensure_submodule

    ensure_submodule()
    cmd_vtm_build()

    decoder = find_vtm_decoder()
    if not decoder.exists():
        print(f"ERROR: VTM DecoderApp was not found after build: {decoder}")
        sys.exit(1)


def native_build(debug: bool, test: bool = True) -> None:
    """Build native standalone module, optionally run tests."""
    build_type = "Debug" if debug else "Release"

    if test:
        ensure_analysis_test_tools()

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
