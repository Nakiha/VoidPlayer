"""Platform packaging staging commands."""

from __future__ import annotations

import fnmatch
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from .check_release_compliance import check_source_tree, check_stage
from .flutter_app import flutter_build, flutter_build_macos
from .macos_ffmpeg import ffmpeg_runtime_dylibs, ffmpeg_runtime_symlinks
from .paths import (
    MACOS_PACKAGE_DIR,
    MACOS_PACKAGE_STAGE_DIR,
    MACOS_FFMPEG_ROOT,
    MACOS_INSTALLER_DIR,
    MACOS_RELEASE_DOCS_DIR,
    ROOT,
    WINDOWS_BUILD_DIR,
    WINDOWS_FFMPEG_ROOT,
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
    "*.vac",
    "*.vck",
}

BUILD_ONLY_FILE_PATTERNS = {
    "*.exp",
    "*.lib",
    "*.pdb",
}

MACOS_RELEASE_ENTITLEMENTS = ROOT / "macos" / "Runner" / "Release.entitlements"


def cmd_package(args) -> None:
    """Build and stage a clean platform package input directory."""
    if sys.platform == "darwin":
        _cmd_package_macos(args)
        return
    if sys.platform == "win32":
        _cmd_package_windows(args)
        return
    print("ERROR: dev.py package is only supported on Windows and macOS.")
    sys.exit(1)


def _cmd_package_windows(args) -> None:
    if sys.platform != "win32":
        print("ERROR: Windows packaging is only supported on Windows.")
        sys.exit(1)

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
    _copy_compliance_docs(stage_dir)
    _copy_windows_ffmpeg_compliance(stage_dir)

    removed = _remove_build_only_artifacts(stage_dir)
    _assert_no_mutable_artifacts(stage_dir, "package staging")
    _assert_no_build_only_artifacts(stage_dir)
    _assert_release_compliance(stage_dir)

    print(f"\nPackage staging ready: {stage_dir}")
    if removed:
        print(f"Removed {removed} build-only artifact(s) from staging.")

    if args.installer:
        _compile_inno_installer(args.iscc, stage_dir)
    else:
        print("Use this directory as the installer input; do not package runner\\Release directly.")


def _cmd_package_macos(args) -> None:
    if args.debug:
        print("ERROR: package currently supports release builds only")
        sys.exit(1)

    app_bundle = ROOT / "build" / "macos" / "Build" / "Products" / "Release" / "VoidPlayer.app"
    stage_dir = MACOS_PACKAGE_STAGE_DIR
    stage_app = stage_dir / "VoidPlayer.app"

    header("Prepare macOS package staging")
    _remove_tree(MACOS_PACKAGE_DIR)

    if not args.no_build:
        _remove_tree(app_bundle)
        flutter_build_macos(debug=False)

    if not app_bundle.exists():
        print(f"ERROR: release app bundle not found: {app_bundle}")
        sys.exit(1)

    _assert_no_mutable_artifacts(app_bundle, "macOS release app")

    print(f"Copy package input: {app_bundle} -> {stage_app}")
    shutil.copytree(app_bundle, stage_app, symlinks=True)
    _copy_release_docs(stage_dir, MACOS_RELEASE_DOCS_DIR)
    _copy_compliance_docs(stage_dir)
    _copy_macos_ffmpeg_compliance(stage_dir)
    _copy_compliance_docs(stage_app / "Contents" / "Resources")

    _assert_no_mutable_artifacts(stage_dir, "macOS package staging")
    _assert_release_compliance(stage_dir)
    _assert_macos_app_compliance(stage_app)
    _assert_macos_nested_code_layout(stage_app)
    _verify_macos_linkage(stage_app)
    sign_identity = args.macos_sign_identity or os.environ.get("VOIDPLAYER_MACOS_SIGN_IDENTITY")
    _sign_macos_app(stage_app, sign_identity)
    _verify_macos_codesign(stage_app)
    if sign_identity:
        _verify_macos_gatekeeper(stage_app)

    print(f"\nmacOS package staging ready: {stage_dir}")
    if args.installer:
        dmg = _create_macos_dmg(stage_dir)
        if args.macos_notarize:
            notary_profile = (
                args.macos_notary_profile
                or os.environ.get("VOIDPLAYER_MACOS_NOTARY_PROFILE")
            )
            _notarize_macos_dmg(dmg, notary_profile)
    else:
        print("Use this directory as the DMG input, or pass --installer to create a local DMG.")


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


def _copy_release_docs(stage_dir: Path, source_dir: Path | None = None) -> None:
    release_docs_dir = source_dir or WINDOWS_RELEASE_DOCS_DIR
    if not release_docs_dir.exists():
        return

    docs_dest = stage_dir / "docs"
    print(f"Copy release docs: {release_docs_dir} -> {docs_dest}")
    shutil.copytree(release_docs_dir, docs_dest, dirs_exist_ok=True)


def _copy_compliance_docs(stage_dir: Path) -> None:
    docs_dest = stage_dir / "docs"
    docs_dest.mkdir(parents=True, exist_ok=True)
    files = [
        (ROOT / "LICENSE", docs_dest / "LICENSE"),
        (ROOT / "THIRD_PARTY_NOTICES.md", docs_dest / "THIRD_PARTY_NOTICES.md"),
        (ROOT / "native" / "THIRD_PARTY_NATIVE.md", docs_dest / "THIRD_PARTY_NATIVE.md"),
    ]
    for src, dest in files:
        print(f"Copy compliance doc: {src} -> {dest}")
        shutil.copy2(src, dest)


def _copy_macos_ffmpeg_compliance(stage_dir: Path) -> None:
    ffmpeg_root = MACOS_FFMPEG_ROOT
    files = [
        (ffmpeg_root / "README.txt", stage_dir / "README.txt"),
        (ffmpeg_root / "VOIDPLAYER_BUILD.md", stage_dir / "VOIDPLAYER_BUILD.md"),
        (ffmpeg_root / "voidplayer-ffmpeg-manifest.json", stage_dir / "voidplayer-ffmpeg-manifest.json"),
    ]
    for src, dest in files:
        print(f"Copy macOS FFmpeg doc: {src} -> {dest}")
        shutil.copy2(src, dest)

    licenses_src = ffmpeg_root / "LICENSES"
    licenses_dest = stage_dir / "LICENSES"
    print(f"Copy macOS FFmpeg licenses: {licenses_src} -> {licenses_dest}")
    shutil.copytree(licenses_src, licenses_dest, dirs_exist_ok=True)


def _copy_windows_ffmpeg_compliance(stage_dir: Path) -> None:
    ffmpeg_root = WINDOWS_FFMPEG_ROOT
    files = [
        (ffmpeg_root / "README.txt", stage_dir / "README.txt"),
        (ffmpeg_root / "voidplayer-ffmpeg-manifest.json", stage_dir / "voidplayer-ffmpeg-manifest.json"),
    ]
    for src, dest in files:
        print(f"Copy Windows FFmpeg doc: {src} -> {dest}")
        shutil.copy2(src, dest)

    licenses_src = ffmpeg_root / "LICENSES"
    licenses_dest = stage_dir / "LICENSES"
    print(f"Copy Windows FFmpeg licenses: {licenses_src} -> {licenses_dest}")
    shutil.copytree(licenses_src, licenses_dest, dirs_exist_ok=True)


def _assert_release_compliance(stage_dir: Path) -> None:
    try:
        check_source_tree()
        check_stage(stage_dir)
    except RuntimeError as exc:
        print(f"\nERROR: release compliance smoke failed: {exc}")
        sys.exit(1)


def _assert_macos_app_compliance(stage_app: Path) -> None:
    resources = stage_app / "Contents" / "Resources"
    ffmpeg_docs = resources / "ThirdParty" / "ffmpeg"
    required = [
        ffmpeg_docs / "README.txt",
        ffmpeg_docs / "VOIDPLAYER_BUILD.md",
        ffmpeg_docs / "voidplayer-ffmpeg-manifest.json",
        ffmpeg_docs / "LICENSES" / "FFmpeg-LICENSE.md",
        resources / "docs" / "LICENSE",
        resources / "docs" / "THIRD_PARTY_NOTICES.md",
        resources / "docs" / "THIRD_PARTY_NATIVE.md",
    ]
    for path in required:
        if not path.is_file():
            print(f"\nERROR: macOS app compliance file missing: {path}")
            sys.exit(1)


def _assert_macos_nested_code_layout(stage_app: Path) -> None:
    helpers_analyzer = (
        stage_app
        / "Contents"
        / "Helpers"
        / "ffmpeg-analysis"
        / "void_ffmpeg_analyzer"
    )
    if not helpers_analyzer.is_file():
        print(f"\nERROR: macOS FFmpeg analyzer helper missing: {helpers_analyzer}")
        sys.exit(1)
    if not os.access(helpers_analyzer, os.X_OK):
        print(f"\nERROR: macOS FFmpeg analyzer helper is not executable: {helpers_analyzer}")
        sys.exit(1)

    legacy_tools_dir = stage_app / "Contents" / "MacOS" / "tools"
    if legacy_tools_dir.exists():
        print(f"\nERROR: legacy macOS helper location is present: {legacy_tools_dir}")
        print("Place bundled helper executables under Contents/Helpers.")
        sys.exit(1)


def _verify_macos_linkage(stage_app: Path) -> None:
    header("Verify macOS app linkage")
    executable = stage_app / "Contents" / "MacOS" / "VoidPlayer"
    frameworks = stage_app / "Contents" / "Frameworks"
    ffmpeg_lib_dir = MACOS_FFMPEG_ROOT / "lib"
    ffmpeg_dylibs = ffmpeg_runtime_dylibs(ffmpeg_lib_dir)
    ffmpeg_symlinks = ffmpeg_runtime_symlinks(ffmpeg_lib_dir)
    required_loads = {f"@rpath/{name}" for name in ffmpeg_dylibs}

    for name in ffmpeg_dylibs:
        path = frameworks / name
        if not path.is_file():
            print(f"\nERROR: bundled FFmpeg dylib missing: {path}")
            sys.exit(1)

    for name in ffmpeg_symlinks:
        path = frameworks / name
        if not path.is_symlink():
            print(f"\nERROR: bundled FFmpeg dylib symlink missing: {path}")
            sys.exit(1)

    executable_loads = set(_otool_libraries(executable))
    missing = sorted(required_loads - executable_loads)
    if missing:
        print("\nERROR: staged app executable is missing FFmpeg @rpath loads:")
        for name in missing:
            print(f"  - {name}")
        sys.exit(1)

    _assert_no_developer_paths(executable, executable_loads)
    for name in ffmpeg_dylibs:
        dylib = frameworks / name
        loads = _otool_libraries(dylib)
        if not loads or loads[0] != f"@rpath/{name}":
            actual = loads[0] if loads else "<none>"
            print(f"\nERROR: {dylib.name} has unexpected install name: {actual}")
            sys.exit(1)
        _assert_ffmpeg_deps_use_rpath(dylib, loads)
        _assert_no_developer_paths(dylib, loads)


def _otool_libraries(binary: Path) -> list[str]:
    print(f"> otool -L {binary}")
    result = subprocess.run(
        ["otool", "-L", str(binary)],
        cwd=str(ROOT),
        check=True,
        capture_output=True,
        text=True,
    )
    libraries: list[str] = []
    for line in result.stdout.splitlines()[1:]:
        stripped = line.strip()
        if not stripped:
            continue
        libraries.append(stripped.split(" (", 1)[0])
    return libraries


def _assert_ffmpeg_deps_use_rpath(binary: Path, loads: list[str]) -> None:
    for library in loads[1:]:
        name = Path(library).name
        if name.startswith(("libav", "libswresample")) and not library.startswith("@rpath/"):
            print(f"\nERROR: {binary.name} has non-rpath FFmpeg dependency: {library}")
            sys.exit(1)


def _assert_no_developer_paths(binary: Path, loads: list[str]) -> None:
    forbidden = (str(ROOT), "/.toolchains/ffmpeg/", "/native/build", "/build/macos/")
    for library in loads:
        if any(marker in library for marker in forbidden):
            print(f"\nERROR: {binary.name} has developer-machine linkage path: {library}")
            sys.exit(1)


def _verify_macos_codesign(stage_app: Path) -> None:
    header("Verify macOS app signature")
    run(["codesign", "--verify", "--deep", "--strict", str(stage_app)], cwd=str(ROOT))


def _verify_macos_gatekeeper(stage_app: Path) -> None:
    header("Verify macOS Gatekeeper assessment")
    run(["spctl", "-a", "-vv", "--type", "execute", str(stage_app)], cwd=str(ROOT))


def _run_macos_codesign(
    path: Path,
    identity: str,
    *,
    developer_id: bool,
    entitlements: Path | None = None,
) -> None:
    cmd = [
        "codesign",
        "--force",
        "--sign",
        identity,
    ]
    if developer_id:
        cmd.extend(["--options", "runtime", "--timestamp"])
    else:
        cmd.append("--timestamp=none")
    if entitlements is not None:
        cmd.extend(["--entitlements", str(entitlements)])
    cmd.append(str(path))
    run(cmd, cwd=str(ROOT))


def _is_machosignable_file(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    result = subprocess.run(
        ["file", "-b", str(path)],
        cwd=str(ROOT),
        check=False,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0 and "Mach-O" in result.stdout


def _macos_nested_code_items(stage_app: Path) -> list[Path]:
    contents = stage_app / "Contents"
    frameworks = contents / "Frameworks"
    search_roots = [
        frameworks,
        contents / "PlugIns",
        contents / "XPCServices",
        contents / "Helpers",
    ]
    items: list[Path] = []
    covered_frameworks: set[Path] = set()

    if frameworks.exists():
        for framework in sorted(frameworks.rglob("*.framework")):
            if framework.is_dir():
                items.append(framework)
                covered_frameworks.add(framework)

    for root in search_roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if any(_is_relative_to(path, framework) for framework in covered_frameworks):
                continue
            if path.is_dir() and path.suffix in {".app", ".appex", ".bundle", ".framework", ".xpc"}:
                items.append(path)
            elif path.suffix in {".dylib", ".so"} or _is_machosignable_file(path):
                items.append(path)

    main_executable = contents / "MacOS" / "VoidPlayer"
    macos_dir = contents / "MacOS"
    if macos_dir.exists():
        for path in sorted(macos_dir.rglob("*")):
            if path == main_executable:
                continue
            if _is_machosignable_file(path):
                items.append(path)

    unique_items = sorted(set(items), key=lambda item: len(item.parts), reverse=True)
    return unique_items


def _sign_macos_app_inside_out(stage_app: Path, identity: str, developer_id: bool) -> None:
    for item in _macos_nested_code_items(stage_app):
        _run_macos_codesign(item, identity, developer_id=developer_id)
    _run_macos_codesign(
        stage_app,
        identity,
        developer_id=developer_id,
        entitlements=MACOS_RELEASE_ENTITLEMENTS,
    )


def _sign_macos_app(stage_app: Path, identity: str | None) -> None:
    if not identity:
        header("Ad-hoc sign staged macOS app")
        _sign_macos_app_inside_out(stage_app, "-", developer_id=False)
        return

    header("Developer ID sign staged macOS app")
    _sign_macos_app_inside_out(stage_app, identity, developer_id=True)


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


def _create_macos_dmg(stage_dir: Path) -> Path:
    header("Create macOS DMG")
    _remove_tree(MACOS_INSTALLER_DIR)
    MACOS_INSTALLER_DIR.mkdir(parents=True, exist_ok=True)

    version = _read_pubspec_version()
    app = stage_dir / "VoidPlayer.app"
    arch = _macos_app_arch_label(app)
    dmg = MACOS_INSTALLER_DIR / f"VoidPlayer-{version}-macos-{arch}.dmg"
    run([
        "hdiutil",
        "create",
        "-volname",
        "VoidPlayer",
        "-srcfolder",
        str(stage_dir),
        "-ov",
        "-format",
        "UDZO",
        str(dmg),
    ], cwd=str(ROOT))

    if not dmg.exists():
        print(f"ERROR: DMG was not created: {dmg}")
        sys.exit(1)

    print(f"\nDMG ready: {dmg}")
    print("Pass --macos-notarize with a notarytool profile to notarize and staple this DMG.")
    return dmg


def _macos_app_arch_label(stage_app: Path) -> str:
    executable = stage_app / "Contents" / "MacOS" / "VoidPlayer"
    result = subprocess.run(
        ["lipo", "-archs", str(executable)],
        cwd=str(ROOT),
        check=True,
        capture_output=True,
        text=True,
    )
    archs = sorted(result.stdout.split())
    if archs == ["arm64"]:
        return "arm64"
    if archs == ["x86_64"]:
        return "x64"
    if archs == ["arm64", "x86_64"]:
        return "universal"
    if not archs:
        return "unknown"
    return "-".join(archs)


def _notarize_macos_dmg(dmg: Path, notary_profile: str | None) -> None:
    header("Notarize macOS DMG")
    if not notary_profile:
        print("ERROR: macOS notarization requires a notarytool keychain profile.")
        print("Create one with xcrun notarytool store-credentials, then pass:")
        print("  python dev.py package --installer --macos-notarize --macos-notary-profile PROFILE")
        print("or set VOIDPLAYER_MACOS_NOTARY_PROFILE=PROFILE.")
        sys.exit(1)

    run([
        "xcrun",
        "notarytool",
        "submit",
        str(dmg),
        "--keychain-profile",
        notary_profile,
        "--wait",
    ], cwd=str(ROOT))
    run(["xcrun", "stapler", "staple", str(dmg)], cwd=str(ROOT))
    run(["xcrun", "stapler", "validate", str(dmg)], cwd=str(ROOT))
    run([
        "spctl",
        "-a",
        "-t",
        "open",
        "--context",
        "context:primary-signature",
        "-v",
        str(dmg),
    ], cwd=str(ROOT))
    print(f"\nNotarized and stapled DMG ready: {dmg}")


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
