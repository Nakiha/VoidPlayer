# VoidPlayer Toolchains

VoidPlayer pins build-critical external tools with lock files in this directory.

`flutter.lock.json` is the source of truth for the Flutter SDK fork required by
HDR/native-compositor builds. Dev commands that invoke Flutter check
the active SDK against this lock before building or testing.

`FLUTTER_FORK_PATCHES.md` records the fork patch inventory and upgrade flow.

`ffmpeg.lock.json` pins the runtime/dev FFmpeg artifacts restored from
`Nakiha/VoidPlayer-FFmpeg-Build`. Restore scripts hydrate those artifacts under
`.toolchains/ffmpeg/`; the hydrated SDKs are local build inputs and are not
committed to this repository.

Useful commands:

```bash
python dev.py toolchain doctor
python dev.py toolchain bootstrap-flutter
```

`python dev.py toolchain bootstrap-flutter` installs the pinned Flutter checkout
under `.toolchains/flutter` and downloads the locked debug/release local engine
archives for platforms that need them. `dev_config.local.json` may still point
to a custom SDK checkout, but the checkout must match the locked revision and
patch markers.

The human-readable `flutterVersion` reported by Flutter can vary between an
official checkout and a fork checkout. The hard lock is the framework revision,
engine revision, Dart SDK version, clean git checkout, and patch markers.

macOS Flutter runner builds also require the local engine archives listed in
`flutter.lock.json` under `macosLocalEngineArtifacts`. Publish those archives as
assets on the `macosLocalEngineReleaseTag` `VoidPlayer-Flutter` release. Use
`scripts/ci/package_flutter_macos_engine.sh debug` and `release` from a machine
with the matching local engine `out/` directories, then copy the printed SHA-256
values into the lock before pushing VoidPlayer changes.

Windows Flutter runner builds require the local engine archives listed under
`windowsLocalEngineArtifacts`. Publish those zip files as assets on the
`windowsEngineReleaseTag` `VoidPlayer-Flutter` release. Use
`scripts/ci/package_flutter_windows_engine.ps1 -Mode debug` and `release` from a
machine with matching `host_debug_unopt` and `host_release` engine outputs, then
copy the printed SHA-256 values into the lock.
