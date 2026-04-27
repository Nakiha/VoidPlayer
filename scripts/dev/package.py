"""Windows packaging staging commands."""

from __future__ import annotations

import fnmatch
import re
import shutil
import sys
from pathlib import Path

from .flutter_app import flutter_build
from .paths import (
    ROOT,
    WINDOWS_BUILD_DIR,
    WINDOWS_INNO_SCRIPT,
    WINDOWS_INSTALLER_DIR,
    WINDOWS_PACKAGE_DIR,
    WINDOWS_PACKAGE_STAGE_DIR,
    WINDOWS_RELEASE_DOCS_DIR,
)
from .process import header, run


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
    "*.log",
    "*.tmp",
    "*.vbi",
    "*.vbs1",
    "*.vbs2",
    "*.vbt",
}

BUILD_ONLY_FILE_PATTERNS = {
    "*.exp",
    "*.lib",
    "*.pdb",
}


def cmd_package(args) -> None:
    """Build and stage a clean Windows package input directory."""
    if args.debug:
        print("ERROR: package currently supports release builds only")
        sys.exit(1)

    release_dir = WINDOWS_BUILD_DIR / "Release"
    stage_dir = WINDOWS_PACKAGE_STAGE_DIR

    header("Prepare Windows package staging")
    _remove_tree(WINDOWS_PACKAGE_DIR)

    if not args.no_build:
        _remove_tree(release_dir)
        flutter_build(debug=False)

    if not release_dir.exists():
        print(f"ERROR: release output not found: {release_dir}")
        sys.exit(1)

    _assert_no_mutable_artifacts(release_dir, "release output")

    print(f"Copy package input: {release_dir} -> {stage_dir}")
    shutil.copytree(release_dir, stage_dir)
    _copy_release_docs(stage_dir)

    removed = _remove_build_only_artifacts(stage_dir)
    _assert_no_mutable_artifacts(stage_dir, "package staging")
    _assert_no_build_only_artifacts(stage_dir)

    print(f"\nPackage staging ready: {stage_dir}")
    if removed:
        print(f"Removed {removed} build-only artifact(s) from staging.")

    if args.installer:
        _compile_inno_installer(args.iscc, stage_dir)
    else:
        print("Use this directory as the installer input; do not package runner\\Release directly.")


def _remove_tree(path: Path) -> None:
    if not path.exists():
        return

    build_root = (ROOT / "build").resolve()
    resolved = path.resolve()
    if not _is_relative_to(resolved, build_root):
        raise RuntimeError(f"Refusing to remove path outside build directory: {path}")

    print(f"Remove stale directory: {path}")
    shutil.rmtree(path)


def _assert_no_mutable_artifacts(root: Path, label: str) -> None:
    offenders = list(_find_mutable_artifacts(root))
    if not offenders:
        return

    print(f"\nERROR: {label} contains runtime/user artifacts:")
    for path in offenders[:30]:
        print(f"  - {path.relative_to(root)}")
    if len(offenders) > 30:
        print(f"  ... and {len(offenders) - 30} more")
    print("\nRefusing to continue. Rebuild into a clean output directory before packaging.")
    sys.exit(1)


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


def _remove_build_only_artifacts(root: Path) -> int:
    removed = 0
    for path in sorted(root.rglob("*"), reverse=True):
        if not path.is_file():
            continue
        name = path.name.lower()
        if any(fnmatch.fnmatch(name, pattern) for pattern in BUILD_ONLY_FILE_PATTERNS):
            path.unlink()
            removed += 1
    return removed


def _copy_release_docs(stage_dir: Path) -> None:
    if not WINDOWS_RELEASE_DOCS_DIR.exists():
        return

    docs_dest = stage_dir / "docs"
    print(f"Copy release docs: {WINDOWS_RELEASE_DOCS_DIR} -> {docs_dest}")
    shutil.copytree(WINDOWS_RELEASE_DOCS_DIR, docs_dest, dirs_exist_ok=True)


def _assert_no_build_only_artifacts(root: Path) -> None:
    offenders: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        name = path.name.lower()
        if any(fnmatch.fnmatch(name, pattern) for pattern in BUILD_ONLY_FILE_PATTERNS):
            offenders.append(path)

    if not offenders:
        return

    print("\nERROR: package staging contains build-only artifacts:")
    for path in offenders[:30]:
        print(f"  - {path.relative_to(root)}")
    sys.exit(1)


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _compile_inno_installer(iscc_arg: str | None, stage_dir: Path) -> None:
    header("Compile Inno Setup installer")

    if not WINDOWS_INNO_SCRIPT.exists():
        print(f"ERROR: Inno Setup script not found: {WINDOWS_INNO_SCRIPT}")
        sys.exit(1)

    iscc = _find_iscc(iscc_arg)
    if iscc is None:
        print("ERROR: ISCC.exe was not found.")
        print("Install Inno Setup, then retry:")
        print("  winget install --id JRSoftware.InnoSetup -e -s winget")
        print("Or pass the compiler path explicitly:")
        print("  python dev.py package --installer --iscc \"C:\\Program Files (x86)\\Inno Setup 6\\ISCC.exe\"")
        sys.exit(1)

    _remove_tree(WINDOWS_INSTALLER_DIR)
    WINDOWS_INSTALLER_DIR.mkdir(parents=True, exist_ok=True)

    version = _read_pubspec_version()
    output_base = f"VoidPlayerSetup-{version}-x64"
    cmd = [
        str(iscc),
        "/Qp",
        f"/DAppVersion={version}",
        f"/DSourceDir={stage_dir}",
        f"/DOutputDir={WINDOWS_INSTALLER_DIR}",
        f"/DOutputBaseFilename={output_base}",
        str(WINDOWS_INNO_SCRIPT),
    ]
    run(cmd, cwd=str(ROOT))

    installer = WINDOWS_INSTALLER_DIR / f"{output_base}.exe"
    if not installer.exists():
        print(f"ERROR: installer was not created: {installer}")
        sys.exit(1)

    print(f"\nInstaller ready: {installer}")


def _find_iscc(iscc_arg: str | None) -> Path | None:
    if iscc_arg:
        path = Path(iscc_arg)
        return path if path.exists() else None

    from_path = shutil.which("ISCC.exe") or shutil.which("iscc.exe")
    if from_path:
        return Path(from_path)

    candidates = [
        Path("C:/Program Files (x86)/Inno Setup 6/ISCC.exe"),
        Path("C:/Program Files/Inno Setup 6/ISCC.exe"),
        Path("C:/Program Files (x86)/Inno Setup 7/ISCC.exe"),
        Path("C:/Program Files/Inno Setup 7/ISCC.exe"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def _read_pubspec_version() -> str:
    pubspec = ROOT / "pubspec.yaml"
    text = pubspec.read_text(encoding="utf-8")
    match = re.search(r"(?m)^version:\s*([0-9]+(?:\.[0-9]+){0,3})(?:\+[0-9A-Za-z.-]+)?\s*$", text)
    if not match:
        print("ERROR: unable to read version from pubspec.yaml")
        sys.exit(1)
    return match.group(1)
