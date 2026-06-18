# Engineering Roadmap

This roadmap keeps project engineering work focused after the macOS and Windows
Flutter fork integrations. The goal is to reduce release risk and build drift
without starting broad source-layout churn.

## Principles

- Prefer reproducibility, diagnostics, and single sources of truth over
  cosmetic reorganization.
- Keep each engineering change independently reviewable and validated.
- Preserve existing `dev.py` command names unless there is a strong reason to
  change the user-facing workflow.
- Move directories only after build and CI behavior are stable.

## Phase 0: Stabilize Current Foundation

Status: done.

- Watch CI fallout from the FFmpeg lock and `VoidPlayerCli` target convergence.
- Fix restore, package, and runner-build issues before starting more cleanup.
- Confirm Windows runner builds `VoidPlayerCli` through the shared CMake helper.
- Confirm macOS package/readiness gates hydrate FFmpeg from `.toolchains/ffmpeg`.

Completed foundation work:

- `toolchains/ffmpeg.lock.json` is the source of truth for pinned FFmpeg
  artifacts.
- Native, Windows runner, macOS runner, and macOS packaging paths resolve
  FFmpeg from `.toolchains/ffmpeg`.
- `VoidPlayerCli` is created through `void_add_analysis_cli`.
- Release compliance checks are platform-aware and no longer require both
  platform packages in every CI job.

Validation:

```bash
python dev.py gate repo-hygiene
python dev.py test --native-only
python dev.py gate macos-ui-smoke
python dev.py gate windows-preservation
```

Run platform-specific gates where the platform and local engine requirements
are available.

## Phase 1: Toolchain Doctor

Status: done.

Add one visible command that tells contributors whether the pinned toolchains
are usable.

- Extend `python dev.py toolchain doctor` to report FFmpeg lock metadata.
- Check whether `.toolchains/ffmpeg/windows-x64` and
  `.toolchains/ffmpeg/macos-arm64` are hydrated when relevant.
- Print the exact restore command when a package is missing.
- Keep Flutter fork checks in the same command so there is one place to look.

Validation:

```bash
python dev.py toolchain doctor
python dev.py gate repo-hygiene
```

## Phase 2: CI Bootstrap Reuse

Status: done.

Remove duplicated workflow bootstrap logic while keeping workflow names and
gate entry points stable.

- Centralize FFmpeg download/restore calls around `scripts/ci/download_ffmpeg_artifacts.sh`.
- Extract analysis submodule initialization to a small script or composite
  action.
- Prefer shared scripts over large inline YAML blocks.
- Keep workflow diffs boring and auditable.

Completed bootstrap work:

- FFmpeg artifact download and verification goes through
  `scripts/ci/download_ffmpeg_artifacts.sh`.
- Analysis vendor submodule setup goes through
  `scripts/ci/init_analysis_submodules.py`.
- Windows Flutter bootstrap goes through
  `scripts/ci/bootstrap_flutter_toolchain.ps1`; macOS/Linux bootstrap uses
  `scripts/ci/bootstrap_flutter_toolchain.sh`.

Validation:

```bash
ruby -e 'require "yaml"; ARGV.each { |p| YAML.load_file(p) }' .github/workflows/*.yml
python dev.py gate repo-hygiene
```

## Phase 3: CMake Source-List Modules

Status: done.

Reduce target drift without moving source files.

- Split long source lists into module-owned CMake files.
- Keep executable/library targets stable.
- Make Windows runner, native standalone, and macOS runner include the same
  shared target helpers wherever behavior should match.
- Avoid source directory moves in this phase.

Completed CMake work:

- Native source lists live in `native/cmake/NativeSources*.cmake`.
- Shared target helpers cover `VoidPlayerCli`, Flutter native runner
  integration, and Windows backend smoke targets.
- Source files stayed in place.

Validation:

```bash
python dev.py test --native-only
python dev.py build --native
```

## Phase 4: Test Layout Cleanup

Status: done.

Only after CI and toolchain behavior are stable, make test layout easier to
review.

- Move Flutter tests from `test/unit/` into source-domain directories.
- Move native tests out of `native/tests/renderer/` into domain directories.
- Keep test names and behavior unchanged during the first move.
- Update CI from `flutter test test/unit` to `flutter test test` in the same
  change.

Completed test-layout work:

- Flutter tests moved out of `test/unit/` into source-domain directories under
  `test/`.
- Windows native renderer tests moved out of the catch-all
  `native/tests/renderer/` directory into domain directories such as
  `native/tests/audio/`, `native/tests/decode/`, `native/tests/ffi/`, and
  `native/tests/windows/`.
- CI and `dev.py` now run `flutter test test`.
- Test names and behavior are unchanged.

Validation:

```bash
python dev.py test
python dev.py gate pr-fast
```

## Phase 5: Release Stabilization And Fork Guardrails

Status: in progress.

Keep the macOS and Windows Flutter fork integrations auditable while the
project moves toward release readiness.

- Protect `toolchains/flutter.lock.json` as the source of truth for the fork
  checkout, local-engine artifacts, and required patch markers.
- Keep `toolchains/FLUTTER_FORK_PATCHES.md` synchronized with the lock whenever
  the fork ref, commit, engine revision, Dart SDK, or local-engine release tag
  changes.
- Make fork drift visible through a cheap static gate before a developer reaches
  a platform build failure.
- Treat ordinary Flutter SDK fallback as non-evidence for compositor-surface
  changes.

Current guardrails:

- `python dev.py gate flutter-fork-protection`
- `python dev.py gate macos-platform-protection`
- `python dev.py gate windows-fork-protection`
- `python dev.py gate repo-hygiene`

Completed platform-protection work:

- Flutter fork lock, local-engine artifacts, patch markers, and workflow
  bootstrap paths are checked by `flutter-fork-protection`.
- Windows DComp/source-projection/high-refresh/overlay native tests, D3D11
  canaries, and preservation UI profiles are checked by
  `windows-fork-protection`.
- macOS FlutterTexture, CVPixelBuffer/IOSurface, renderer-owned Metal
  presentation, VideoToolbox fallback, native Metal canaries, and macOS UI
  profiles are checked by `macos-platform-protection`.

Next candidates:

- Add a release evidence ledger that records commit, Flutter fork lock, FFmpeg
  lock, CI runs, local-engine gates, and known manual hardware gaps for each
  release candidate.
- Pause broad file moves until release evidence exposes a concrete maintenance
  problem.

## Non-Goals For Now

- Do not reorganize `lib/main_window/` into a new architecture.
- Do not move Windows runner or macOS runner files purely for tidiness.
- Do not rename `resources/video/` until UI CSV, CMake, and package references
  are already easy to audit.
- Do not delete UI tests unless a lower-level regression test covers the same
  risk.
