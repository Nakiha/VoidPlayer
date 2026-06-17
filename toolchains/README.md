# VoidPlayer Toolchains

VoidPlayer pins build-critical external tools with lock files in this directory.

`flutter.lock.json` is the source of truth for the Flutter SDK fork required by
HDR/native-compositor builds. Dev commands that invoke Flutter check
the active SDK against this lock before building or testing.

`FLUTTER_FORK_PATCHES.md` records the fork patch inventory and upgrade flow.

Useful commands:

```bash
python dev.py toolchain doctor
python dev.py toolchain bootstrap-flutter
scripts/ci/bootstrap_flutter_macos_engine.sh
```

`dev_config.local.json` may still point to a local SDK checkout, but the checkout
must match the locked revision and patch markers.

The human-readable `flutterVersion` reported by Flutter can vary between an
official checkout and a fork checkout. The hard lock is the framework revision,
engine revision, Dart SDK version, clean git checkout, and patch markers.

macOS Flutter runner builds also require the local engine archives listed in
`flutter.lock.json` under `macosLocalEngineArtifacts`. Publish those archives as
assets on the `macosLocalEngineReleaseTag` `VoidPlayer-Flutter` release. Use
`scripts/ci/package_flutter_macos_engine.sh debug` and `release` from a machine
with the matching local engine `out/` directories, then copy the printed SHA-256
values into the lock before pushing VoidPlayer changes.
