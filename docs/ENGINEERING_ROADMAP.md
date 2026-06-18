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

Status: in progress.

- Watch CI fallout from the FFmpeg lock and `VoidPlayerCli` target convergence.
- Fix restore, package, and runner-build issues before starting more cleanup.
- Confirm Windows runner builds `VoidPlayerCli` through the shared CMake helper.
- Confirm macOS package/readiness gates hydrate FFmpeg from `.toolchains/ffmpeg`.

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

Status: in progress.

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

Remove duplicated workflow bootstrap logic while keeping workflow names and
gate entry points stable.

- Centralize FFmpeg download/restore calls around `scripts/ci/download_ffmpeg_artifacts.sh`.
- Extract analysis submodule initialization to a small script or composite
  action.
- Prefer shared scripts over large inline YAML blocks.
- Keep workflow diffs boring and auditable.

Validation:

```bash
ruby -e 'require "yaml"; ARGV.each { |p| YAML.load_file(p) }' .github/workflows/*.yml
python dev.py gate repo-hygiene
```

## Phase 3: CMake Source-List Modules

Reduce target drift without moving source files.

- Split long source lists into module-owned CMake files.
- Keep executable/library targets stable.
- Make Windows runner, native standalone, and macOS runner include the same
  shared target helpers wherever behavior should match.
- Avoid source directory moves in this phase.

Validation:

```bash
python dev.py test --native-only
python dev.py build --native
```

## Phase 4: Test Layout Cleanup

Only after CI and toolchain behavior are stable, make test layout easier to
review.

- Move Flutter tests from `test/unit/` into source-domain directories.
- Move native tests out of `native/tests/renderer/` into domain directories.
- Keep test names and behavior unchanged during the first move.
- Update CI from `flutter test test/unit` to `flutter test test` in the same
  change.

Validation:

```bash
python dev.py test
python dev.py gate pr-fast
```

## Non-Goals For Now

- Do not reorganize `lib/main_window/` into a new architecture.
- Do not move Windows runner or macOS runner files purely for tidiness.
- Do not rename `resources/video/` until UI CSV, CMake, and package references
  are already easy to audit.
- Do not delete UI tests unless a lower-level regression test covers the same
  risk.
