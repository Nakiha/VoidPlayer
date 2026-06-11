# VoidPlayer Toolchains

VoidPlayer pins build-critical external tools with lock files in this directory.

`flutter.lock.json` is the source of truth for the Flutter SDK fork required by
HDR/native-compositor exploration builds. Dev commands that invoke Flutter check
the active SDK against this lock before building or testing.

`FLUTTER_FORK_PATCHES.md` records the fork patch inventory and upgrade flow.

Useful commands:

```bash
python dev.py toolchain doctor
python dev.py toolchain bootstrap-flutter
```

`dev_config.local.json` may still point to a local SDK checkout, but the checkout
must match the locked revision and patch markers.

The human-readable `flutterVersion` reported by Flutter can vary between an
official checkout and a fork checkout. The hard lock is the framework revision,
engine revision, Dart SDK version, clean git checkout, and patch markers.
