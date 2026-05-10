# Native Refactor Todo

Source reviews:

- `build/chat_native_adv.md` - newer static review; mostly matches current code.
- `build/chat_old_native_adv.md` - older static review; useful for direction, but some items are already fixed or moved.

This file is the active tracker for native-side cleanup. Before each round, confirm the issue still exists in the current tree. After each round, update this file and run the validation command required by `AGENTS.md`.

## Current Findings

Confirmed against current code:

- [x] Renderer C FFI raw `NativePlayer*` live-set was present and has been replaced in Round 1 with a pinned handle state registry.
- [x] Analysis FFI already uses `shared_ptr` pinning for handle state; Round 1 reused that model for renderer FFI.
- [x] Logging and crash handling are physically split into `native/common/`; Round 2 made console code page changes, environment level overrides, global flush, and runner crash hooks explicit opt-in behavior.
- [x] Windows runner installs native logging and crash handling during plugin construction, but now does so with explicit app-layer options rather than relying on reusable-library defaults.
- [x] CI native config matrix configured only; Round 3 changed it to configure + build and added an FFI ABI smoke for the tests/FFI combination.
- [x] Dependency fetching previously preferred local build cache and tags; Round 4 made local cache opt-in, pinned FetchContent commits, and added a native third-party manifest.
- [x] Analysis FFI ABI was behind renderer FFI; Round 5 added v2 size/version wrappers, thread-local last error, and atomic PTS callback while preserving legacy Dart structs.
- [x] Renderer remains a large coordinator; Round 7 tightened the lock/thread contract and documented the next safe split boundaries.

## Round 1 - Renderer FFI Handle Lifetime

Status: done

- [x] Replace raw `NativePlayer*` FFI handle registry with a heap handle state stored in a `shared_ptr` registry.
- [x] Make every player API pin the handle state before using `NativePlayer`.
- [x] Serialize calls on a per-handle mutex; make destroy unregister first, then wait for any in-flight call before shutdown/release.
- [x] Keep `naki_vr_player_t` opaque and ABI-compatible.
- [x] Document the FFI lifetime/concurrency contract in `FFI_AND_BINDINGS.md`.
- [x] Add/extend native FFI tests for destroyed handle behavior and concurrent destroy/call smoke.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10.

## Round 2 - Logging / Crash Runtime Boundary

Status: done

- [x] Split logging options into library-safe defaults and app opt-in process-global behavior.
- [x] Stop `configure_logging()` from unconditionally changing Windows console code pages.
- [x] Replace generic `SPDLOG_LEVEL` override with VoidPlayer-specific opt-in behavior or explicit app-side parsing.
- [x] Avoid starting/changing process-global spdlog flush policy from the reusable library default path.
- [x] Keep host sinks intact and keep native-owned sinks replaceable on reconfigure.
- [x] Replace app/plugin `install_crash_handler(crash_dir)` usage with explicit `WindowsCrashHandlerConfig` choices.
- [x] Update tests for logging sink preservation, env override opt-in, and crash handler install/remove behavior.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10.
- [x] Runner validation because `windows/runner/video_renderer_plugin.cpp` changed: `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv` passed on 2026-05-10.

## Round 3 - CI Build Matrix

Status: done

- [x] Change `.github/workflows/native.yml` matrix job from configure-only to configure + build.
- [x] Build the key `BUILD_PYTHON`, `BUILD_FFI`, `BUILD_TESTS` combinations already listed in the matrix.
- [x] Add a package/FFI smoke where a clean output loads the FFI DLL and runs ABI validation.
- [x] Keep heavy external analysis tests separate from normal GitHub CI by setting `BUILD_ANALYSIS_TESTS=OFF` in the matrix; the main native test job still owns the normal GitHub native test path.
- [x] Validate locally with the closest available command: `python dev.py test --native-only` passed on 2026-05-10 before the workflow-only edit.

## Round 4 - Dependency Lock / Native Third-Party Manifest

Status: done

- [x] Make local dependency cache usage explicit opt-in instead of default priority.
- [x] Pin FetchContent dependencies by commit SHA or URL hash.
- [x] Add/update a native third-party manifest for FFmpeg, zstd, spdlog, Catch2, VTM/analyzer tools, including version/source/license/update notes.
- [x] Ensure FFmpeg runtime staging contains machine-checkable license/source-offer metadata by tracking the existing copied `README.txt` and `LICENSE*` files in `native/THIRD_PARTY_NATIVE.md`.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10 after locked FetchContent reconfigure/build.

## Round 5 - Analysis FFI ABI And Error Model

Status: done

- [x] Add size/version-prefixed structs or v2 structs for analysis FFI without breaking current Dart callers.
- [x] Replace naked callback pointer with atomic or locked access.
- [x] Add status/last-error style error reporting for handle APIs.
- [x] Mark singleton/global analysis APIs as legacy by documenting handle APIs as preferred; routing all singleton internals through handle state is left for a future breaking cleanup.
- [x] Document pointer lifetimes for returned thread-local/static buffers, or replace with caller-provided output buffers.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10.
- [x] UI validation because `windows/runner/analysis_ffi.*` changed: `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv` passed on 2026-05-10.

## Round 6 - Parser / Queue Hardening

Status: done

- [x] Add fuzz/property style coverage for VBS4/VBI/VBT/VAC parsers where current parser seams are available: VBS4 now has a generated minimal corrupt-file regression for raw stream trailing bytes; broader coverage remains a future corpus/fuzzer task.
- [x] Add property tests for `BidiRingBuffer` and `PacketQueue` state transitions.
- [x] Confirm parser budgets for untrusted analysis inputs already exist for frames, sections, streams, records, CUs, decoded size, and file ranges; no new budget constants were needed in this round.
- [x] Tighten stream decoder checks where trailing garbage should be rejected.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10.

## Round 7 - Renderer Thread Contract And Further Split

Status: done

- [x] Document lock ordering for `lifecycle_mutex_`, `state_mutex_`, `device_mutex_`, texture mutexes, callbacks, and render thread joins in `THREADING_MODEL.md`.
- [x] Document which APIs can be called from UI/FFI thread, render thread, demux/decode threads, audio paths, and Flutter texture consumer.
- [x] Keep further code splits behind the new contract; this round intentionally avoided a mechanical move because the safest next split needs a focused feature branch and UI texture validation.
- [x] Candidate splits documented: render loop controller, layout controller, analysis overlay renderer, frame capture service, device loss policy.
- [x] Validate by impact area: `python dev.py test --native-only` passed on 2026-05-10. No runner behavior changed in this round.

## Round 8 - Python Binding Ergonomics

Status: done

- [x] Add `py::gil_scoped_release` to blocking operations such as initialize/shutdown/seek/add_track where safe.
- [x] Route Python layout validation through the same core validation used by C FFI.
- [x] Document Python binding limits as demo/dev tooling rather than a stable public ABI.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10; Python import/layout validation smoke passed from `native/build-msvc/dist/python`.

## Round 9 - Seek Edge Cases

Status: done

- [x] Move the H.264 FLV exact-seek SPS/PPS limitation from code comments into docs/user-visible capability notes.
- [x] Decide behavior for now: H.264/FLV exact seek remains best-effort and emits a warning; no automatic SPS/PPS injection or silent downgrade until the bitstream layer can reliably detect missing parameter sets around target IDR.
- [x] Regression asset decision: no legal, small, committed sample exists in this tree for "IDR without repeated SPS/PPS"; `SEEK_STRATEGY.md` now records the required fixture gap for any future behavior change.
- [x] Validate: `python dev.py test --native-only` passed on 2026-05-10. No runner behavior changed in this round.
