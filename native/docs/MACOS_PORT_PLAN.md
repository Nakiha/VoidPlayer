# macOS Port Plan

本文档记录 VoidPlayer 从 Windows-only 播放器演进到 macOS 可用版本的阶段计划。

当前原则：先建立平台边界和可验证路径，再追求性能。macOS 不能在 Runner
plugin 里复制一套播放器业务逻辑；它应复用 native player / demux / decode /
playback orchestration，只替换窗口、音频、硬解、渲染和 Flutter texture bridge。

## Current State

- Flutter/Dart 层仍以 Windows 主窗口为中心，`lib/windows/` 同时承载主窗口、
  analysis 窗口、Win32 FFI、路径打开、窗口管理和主题能力。
- native CMake 已经有 `video_renderer_core` / `video_renderer_lib` 的初步边界，
  但 `VOID_RENDERER_WINDOWS_SOURCES` 仍包含大量平台无关 player/renderer/audio
  pipeline 代码。
- Windows runner 通过 `native/cmake/FlutterNativeTarget.cmake` 直接把 native 源码编进
  `void_player.exe`，并链接 D3D11 / DXGI / WinMM。
- macOS FFmpeg arm64 包已放在 `third_party/ffmpeg`，这是
  `native/cmake/FFmpeg.cmake` 的非 Windows 默认 root。
- 仓库已有 curated macOS runner 基线。`macos/Pods/`、`macos/Flutter/ephemeral/`、
  `macos/build*` 和 Xcode 用户态产物必须保持 ignored/generated 状态。
- Dart 入口已经按平台 deferred 到 Windows/macOS bootstrap；macOS runner 现在注册
  deterministic `video_renderer` / `video_renderer/events` backend。该 backend 已可注册
  synthetic `FlutterTexture` 并返回合成 track/capture 状态，但 diagnostics 仍明确标记
  `available=false`，避免把桥接验证误认为真实播放。

## Non-Goals

- 不在 macOS plugin / MethodChannel 层重写独立播放器。
- 不为了 macOS 引入 `libswscale` / `libyuv` 作为通用 fallback。
- 不在一个提交里同时改 Windows 音频、Dart UI、native target split 和 macOS 渲染。
- 不让 silent no-op 假装功能可用；未实现能力必须有 capability gate 或明确错误。
- 不把 `macos/build*`、`Pods/`、FetchContent `_deps/`、DerivedData 等生成物提交进仓库。

## Target Architecture

```
Dart UI / Actions
  -> platform coordinator
     -> Windows MethodChannel / macOS MethodChannel
        -> thin runner plugin
           -> native player facade
              -> shared media / playback / decode / track pipeline
                 -> platform render backend
                    -> D3D11 shared texture on Windows
                    -> CVPixelBuffer / IOSurface / Metal on macOS
```

Required native target layers:

| Layer | Owns | Platform Notes |
| --- | --- | --- |
| `void_core` | logging contracts, clocks, queues, pure policies | no FFmpeg, no Flutter, no platform SDK |
| `void_media_ffmpeg` | demux, packet/frame lifetime, software decode adapters | uses `third_party/ffmpeg` on macOS |
| `void_player` | NativePlayer facade, playback, track lifecycle, layout state | no MethodChannel ownership |
| `void_render_d3d11` | D3D11 device, shaders, capture, Windows texture bridge | Windows only |
| `void_render_metal` | Metal device, shaders, CVPixelBuffer/IOSurface output | macOS only |
| `void_audio_output` | miniaudio playback callback fed by VoidPlayer-owned decode/mix pipeline | Windows now, macOS later |
| `void_audio_macos` | CoreAudio/miniaudio output implementation | macOS only |
| `void_flutter_windows_plugin` | Windows runner glue | thin adapter |
| `void_flutter_macos_plugin` | macOS runner glue | thin adapter |

The names above are target-direction names. Introduce them only when the dependency edge is real
and covered by a validation command.

## Phase 0: Repository Hygiene And Baseline

Goal: make macOS work safe to start without disturbing Windows release behavior.

Tasks:

- Add `.gitignore` coverage for macOS generated outputs before committing any `macos/` runner:
  `macos/Pods/`, `macos/build/`, `macos/build_native/`, `macos/Flutter/ephemeral/`,
  `macos/.symlinks/`, Xcode user data, and DerivedData-like outputs.
- Decide whether macOS runner files come from `flutter create --platforms=macos .` or a curated
  runner snapshot. Prefer generating into a temporary clean directory and copying curated files
  back; do not run generation on top of the existing untracked `macos/` cache/scaffold.
- Record the macOS toolchain baseline: Flutter version, Xcode version, minimum macOS deployment
  target, target arch (`arm64` first), and codesign expectations.
- Keep Windows FFmpeg and macOS FFmpeg packages separate; only `third_party/ffmpeg/lib/*.dylib`
  should be LFS-managed for current macOS artifacts.
- Decide how macOS CMake resolves third-party headers in offline/reproducible builds. The current
  `Dependencies.cmake` has a Windows-biased local spdlog cache path and a FetchContent fallback;
  the macOS path needs an explicit cache/vendor policy before CI depends on it.

Validation:

- `git status --short` shows no generated macOS directories.
- `flutter --version` and `xcodebuild -version` are recorded in the phase PR/commit notes.
- `git lfs ls-files | rg 'third_party/ffmpeg'` lists only real versioned dylibs.

Exit criteria:

- A clean macOS runner baseline can be committed without generated build/cache artifacts.
- Windows smoke remains unchanged from `origin/main`.

## Phase 1: Flutter Platform Boundary And Deterministic Stubs

Goal: macOS can launch the Flutter UI and every player/analysis entrypoint fails predictably or
returns deterministic stub state.

Status on 2026-05-20:

- Done: `lib/main.dart` no longer imports the Windows bootstrap eagerly. It deferred-loads
  Windows or macOS bootstrap based on `Platform`.
- Done: app-level dependencies for analysis process hosting, system accent watching, fullscreen
  window operations, and pointer-button recovery now flow through platform interfaces under
  `lib/platform/`.
- Done: Windows bootstrap injects the existing Win32-backed implementations, preserving current
  Windows behavior.
- Done: macOS bootstrap injects generic/no-op implementations and a fixed macOS accent color.
- Done: macOS runner registers a deterministic `video_renderer` MethodChannel/EventChannel stub.
- Done: app-data/log paths now resolve to `~/Library/Application Support/VoidPlayer` on macOS
  instead of attempting to create directories below `Contents/MacOS/VoidPlayer`.
- Done: native file picking and path launching have platform interfaces/defaults outside
  Windows-named classes.
- Done: top-level Add Media, Analysis, and empty-viewport affordances now respect Phase 1
  platform capability state.
- Remaining: deeper capability checks are still needed for automation-only commands, viewport
  capture assertions, and settings/actions that may call unavailable backend methods directly.

Tasks:

- [x] Move Windows-specific assumptions behind platform services instead of importing `lib/windows/*`
  from app-level code paths that must run on macOS.
- [x] Define a platform capability surface for:
  player backend, external analysis windows, native viewport capture, system accent/theme, path
  launching, pointer button state, and window effects.
- [ ] Consolidate direct MethodChannel bypasses into platform services. Current examples include
  `lib/app_log.dart` and `lib/windows/native_file_picker.dart` using `video_renderer` directly
  instead of going through the player API/platform boundary.
- [x] Implement macOS stubs for all MethodChannel methods expected by `NativePlayerApi`, including
  destroy, loop range, audible track, capture, presented frame, layout, tracks, diagnostics, and
  error/status calls.
- [x] Hide or disable top-level unavailable UI affordances through capabilities rather than
  silent no-op hosts.
- [ ] Add automation assertions that can verify "macOS backend unavailable" or "stub mode" without
  depending on real playback.

Validation:

- `flutter analyze`
- `flutter test` for platform service unit tests where possible.
- `flutter build macos --debug` or `flutter build macos --release` once the runner exists.
- Current verified subset: `flutter analyze`, `flutter test test/unit/app_paths_test.dart
  test/unit/main_window_controller_injection_test.dart`, `flutter build macos --debug`, and a
  manual macOS launch smoke using Computer Use to confirm the Flutter toolbar/empty state render.
- Not yet covered: real macOS UI automation. The existing `python dev.py ui-test ...` path is
  Windows-oriented and should not be counted as macOS UI coverage.
- Future: a macOS smoke automation script once Codex/UI automation supports macOS windows.

Exit criteria:

- Launching on macOS does not throw `MissingPluginException`.
- All unavailable features expose explicit disabled states or deterministic unsupported errors.
- Windows UI automation smoke still passes with `python dev.py ui-test --build ui_tests/smoke/basic.csv`.

## Phase 2: Native Source Boundary Split

Goal: separate platform-independent playback/decode/layout logic from Windows D3D11/WinMM glue.

Status on 2026-05-20:

- Done: `VOID_PLAYER_PORTABLE_CORE_SOURCES` and `VOID_MEDIA_FFMPEG_SOURCES` split the first
  macOS-buildable native source groups out of `VOID_RENDERER_CORE_SOURCES`.
- Done: non-Windows CMake now creates `void_player_portable_core` and `void_media_ffmpeg`
  internal static libraries instead of building only third-party dependencies.
- Done: `codec_loop.cpp` has a non-Windows FFmpeg path, allowing decode policies, exact seek
  policies, and timestamp rescaling to compile in `void_media_ffmpeg`.
- Verified: `cmake -S native -B native/build-macos-make -G "Unix Makefiles"
  -DBUILD_ANALYSIS=OFF -DBUILD_TESTS=OFF -DBUILD_FFI=OFF -DBUILD_PYTHON=OFF`
  and `cmake --build native/build-macos-make --target void_player_portable_core
  void_media_ffmpeg -- -j2`.
- Note: this machine does not have Ninja installed, so macOS validation should not require
  `-G Ninja` until the toolchain baseline explicitly installs it.
- Remaining: `frame_converter.cpp`, `decoded_frame_publisher.cpp`, and
  `exact_seek_frame_publisher.cpp` still depend on the D3D-oriented converter/hardware provider
  surface and need a renderer-neutral frame publication boundary before moving further.

Tasks:

- Split `VOID_RENDERER_WINDOWS_SOURCES` into:
  `VOID_PLAYER_CORE_SOURCES`, `VOID_RENDERER_COMMON_SOURCES`,
  `VOID_RENDERER_PLATFORM_WINDOWS_SOURCES`, and later
  `VOID_RENDERER_PLATFORM_MACOS_SOURCES`.
- Audit the existing `VOID_RENDERER_CORE_SOURCES` first. It currently contains Windows-specific
  files such as `common/windows_crash_handler.cpp`; "core" must become platform-clean before it is
  used as a macOS build root.
- Move Windows-only headers and libs out of shared files:
  `renderer.cpp` must not include `<windows.h>` / `<mmsystem.h>` directly.
- Break out an explicit render backend interface milestone. `Renderer` currently owns and exposes
  D3D11 backend objects; macOS work should first prove that `Renderer` can compile and run against
  a null/mock backend before a Metal backend is added.
- Keep the `PlaybackController` dependency on `AudioOutput` platform-neutral. If source movement
  exposes backend coupling, pull the audio-output factory boundary forward instead of dragging
  platform audio into shared targets.
- Extract platform adapters for timer resolution, crash handling, audio output, hardware decode,
  render backend creation, and texture output.
- Keep FFmpeg demux/decode code shared; platform-specific hardware decode providers should be
  selected behind a provider interface.
- Update `native/docs/TARGET_BOUNDARIES.md` as targets become real.

Validation:

- Windows: `python dev.py test --native-only`
- Disabled-feature checks for `BUILD_ANALYSIS=OFF`, `BUILD_FFI=OFF`, and `BUILD_TESTS=OFF` when
  touched.
- CMake configure/build on macOS for non-renderer core targets. Use an available generator such as
  `Unix Makefiles`; do not assume Ninja is installed.

Exit criteria:

- A native target containing shared player/media/decode code configures on macOS without D3D11,
  DXGI, WinMM, or Windows crash-handler dependencies.
- Windows native tests pass after each split step.

## Phase 3: macOS Native Build Target

Goal: build a minimal macOS native library that links FFmpeg and exposes the same player facade
surface expected by the runner.

Status on 2026-05-20:

- Done: `macos_media_smoke` links `void_media_ffmpeg`, opens the bundled
  `resources/video/h264_9s_1920x1080.mp4` fixture, and validates video stream metadata/duration.
- Verified: `cmake --build native/build-macos-make --target macos_media_smoke -- -j2`,
  `ctest --test-dir native/build-macos-make -R macos_media_smoke --output-on-failure`, and
  `otool -L native/build-macos-make/macos_media_smoke`.
- Remaining: this is still a CLI/CTest probe. The macOS runner does not yet link or package a
  VoidPlayer native library, and FFmpeg dylibs are not yet copied/signed into the `.app` bundle.

Tasks:

- Add `native/cmake/MacOSNativeTarget.cmake` or equivalent CMake path.
- Link `third_party/ffmpeg/lib/*.dylib` and include `third_party/ffmpeg/include`.
- Copy/sign/package FFmpeg dylibs into the macOS app bundle runtime location.
- Add a minimal CLI or CTest target that opens a local media file, reads metadata/duration, and
  can decode at least one software frame without presenting it.
- Keep analysis optional; macOS native playback should build with `BUILD_ANALYSIS=OFF` first.

Validation:

- `cmake -S native -B native/build-macos -G Ninja -DBUILD_ANALYSIS=OFF -DBUILD_TESTS=ON`
  or the agreed macOS preset.
- `cmake --build native/build-macos`
- `ctest --test-dir native/build-macos --output-on-failure` for portable tests.
- `otool -L` on the built native artifact and bundled FFmpeg dylibs.

Exit criteria:

- macOS native code links against the vendored FFmpeg package.
- Software decode smoke works headlessly.
- No Windows system libraries appear in macOS link output.

## Phase 4: Flutter Texture Bridge MVP

Goal: display a deterministic frame in Flutter on macOS before optimizing the render path.

Status on 2026-05-20:

- Done: macOS runner registers a synthetic `FlutterTexture` backed by a CPU-filled BGRA
  `CVPixelBuffer`.
- Done: `createPlayer`, `addTrack`, `resize`, `destroyPlayer`, `getTracks`, `duration`,
  `currentPts`, and synthetic `captureViewport` now have deterministic macOS behavior for bridge
  validation.
- Done: `ui_tests/macos/synthetic_texture_smoke.csv` exercises the MethodChannel path through the
  existing automation runner and checks synthetic capture metrics. The `ADD_MEDIA` path is a
  synthetic label, not a sandboxed file read.
- Done: manual visual smoke via Computer Use confirmed the Flutter window displays the synthetic
  color bars through the macOS texture path.
- Remaining: add a repeatable macOS screenshot/pixel assertion path, then connect the texture bridge
  to decoded FFmpeg frames instead of generated color bars.
- Risk note: until the real player is connected, track count, duration, `isPlaying`, seek/step, and
  capture metrics are bridge-validation signals only. macOS feature availability must continue to
  use platform capabilities or diagnostics, not plausible synthetic playback state.

Tasks:

- [x] Implement a macOS `FlutterTexture` object whose `copyPixelBuffer` returns a retained current
  `CVPixelBufferRef`.
- [x] Add native-to-runner frame availability notification using Flutter macOS texture registrar.
- [x] Start with CPU-produced `CVPixelBuffer` frames; prove lifecycle, retain/release, frame pacing,
  resize, and shutdown behavior.
- [x] Add a synthetic color-bar/test-pattern source before connecting FFmpeg decode.
- [x] Add synthetic capture metrics for UI-test assertions.
- [ ] Add a macOS-specific screenshot assertion path that samples the real window/texture output.

Validation:

- `flutter build macos --debug`
- Manual launch: synthetic frame appears, resize does not crash, close/reopen does not leak handles.
- Automated screenshot/hash test when macOS UI automation is available.
- Current smoke: copy `ui_tests/macos/synthetic_texture_smoke.csv` into
  `~/Library/Containers/dev.nakiha.voidplayer/Data/tmp/`, then run
  `build/macos/Build/Products/Debug/VoidPlayer.app/Contents/MacOS/VoidPlayer --silent-ui-test
  --test-script <container-script-path>`. The copy is needed because the debug app is sandboxed.

Exit criteria:

- Flutter can show nonblank native-owned pixels on macOS.
- Texture teardown is deterministic and safe across player destroy and window close.

## Phase 5: Software Playback MVP

Goal: play local media on macOS with FFmpeg software decode, CPU color conversion owned by the
existing deterministic color pipeline, and basic audio.

Tasks:

- Connect MethodChannel open/play/pause/seek/destroy to the shared native player facade.
- Implement macOS audio output using CoreAudio or miniaudio behind `AudioOutput`.
- Feed decoded frames into the `CVPixelBuffer` texture bridge.
- Reuse existing timeline, seek, loop, layout, and track state semantics.
- Keep VideoToolbox and Metal acceleration disabled until software playback is stable.

Validation:

- Native portable tests.
- `flutter build macos --release`.
- macOS smoke: open bundled fixture, play/pause, seek, step, close.
- Windows regression: `python dev.py test --native-only` and relevant UI smoke with `--build`.

Exit criteria:

- Single-track H.264/HEVC/AV1 software playback works on macOS with correct duration/current PTS.
- Pause, seek, step, and destroy do not deadlock.
- Windows behavior remains unchanged.

## Phase 6: Metal Render Backend

Goal: replace CPU texture upload/presentation with a Metal-backed renderer while preserving the
shared playback and layout contracts.

Tasks:

- Add `MetalRenderBackend` with the same responsibilities as the D3D11 backend:
  frame presenter, shader constants, layout composition, overlay hooks, capture path, and output
  texture ownership.
- Port shader logic from HLSL to Metal Shading Language with explicit tests for constant-buffer
  layout and color conversion parity.
- Use IOSurface-backed `CVPixelBuffer` / `MTLTexture` where it reduces copies without changing the
  Dart/NativePlayer API.
- Validate multi-track layout, split view, pan/zoom, pixel-size modes, and capture.

Validation:

- Native unit tests for layout and color math.
- Pixel/hash comparison between CPU path and Metal path on deterministic fixtures.
- macOS UI tests for viewport layout and seek presentation.

Exit criteria:

- Multi-track render output matches expected layout and color tolerances.
- Metal path survives resize, track add/remove, seek storms, and player teardown.

## Phase 7: VideoToolbox Hardware Decode

Goal: add macOS hardware decode without changing playback semantics.

Tasks:

- Implement `VideoToolboxDecodeProvider` behind the existing hardware decode provider interface.
- Define frame ownership from FFmpeg `AVFrame` hardware surfaces to Metal/CVPixelBuffer.
- Keep deterministic software fallback for unsupported codecs/pixel formats.
- Validate color parity with software decode for SDR fixtures before enabling by default.

Validation:

- Codec fixtures: H.264, HEVC, AV1 when platform support exists, VP9 if applicable.
- Seek/step/loop tests on both hardware and software decode.
- Color/hash comparison against software baseline.

Exit criteria:

- Hardware decode can be toggled per player.
- Fallback is visible in diagnostics and does not crash or silently change playback state.

## Phase 8: Analysis, Packaging, And Release Readiness

Goal: make macOS behavior explicit for analysis tooling and produce a distributable app bundle.

Tasks:

- Add macOS capability gates for external analysis windows and IPC. If unsupported, UI must say so
  through disabled state/status instead of no-op.
- Decide whether analysis cache generation uses the same native analysis library in-process,
  a macOS helper process, or remains Windows-only for the first macOS release.
- Add codesign/notarization packaging notes and FFmpeg license notice inclusion.
- Update `THIRD_PARTY_NOTICES.md` and release docs for macOS FFmpeg/dav1d.
- Add CI/build automation for macOS once runner and native targets are stable.

Validation:

- `flutter build macos --release`
- App bundle inspection: FFmpeg dylibs present, `otool -L` resolves expected `@rpath` entries.
- Basic launch/open/play smoke on a clean macOS machine.
- Windows release smoke remains green.

Exit criteria:

- A user can install or run the macOS app bundle and play supported local files.
- Unsupported analysis features are clearly gated.
- Licensing and bundled binary provenance are documented.

## Cross-Cutting Requirements

### Testing Matrix

Each phase must choose verification by blast radius:

| Change Type | Required Validation |
| --- | --- |
| CMake source split | Windows native tests plus macOS configure/build for touched targets |
| MethodChannel/API surface | Flutter analyze plus platform-specific launch/stub smoke |
| Texture bridge | macOS build plus nonblank texture verification |
| Decode/playback | Native tests plus seek/timeline/loop smoke on affected platform |
| Render backend | Pixel/hash or screenshot comparison for deterministic fixtures |
| Packaging | Bundle inspection plus fresh-machine smoke |

### Documentation Updates

Update docs in the same commit that changes the contract:

- `native/docs/TARGET_BOUNDARIES.md` when native target ownership changes.
- `native/docs/FFI_AND_BINDINGS.md` when public C ABI or MethodChannel expectations change.
- `lib/doc.md` / `lib/docs/FLUTTER_ARCHITECTURE.md` when platform service ownership changes.
- `THIRD_PARTY_NOTICES.md` when bundled macOS runtime dependencies change.

### Commit Cadence

- One phase may require many commits, but each commit should have one behavioral theme.
- Run the phase's minimum validation before committing.
- Keep Windows regressions separate from macOS enablement fixes unless the same boundary change
  requires both.
- Prefer deterministic stubs and capability gates over speculative partial implementations.

## First Five Implementation Tickets

1. Add macOS `.gitignore` hygiene and commit a clean Flutter macOS runner baseline.
2. Introduce platform capability interfaces in Dart and make macOS unsupported paths explicit.
3. Finish Phase 1 capability gating for player availability, file picking, path launching, and
   external analysis windows.
4. Split native source lists so shared player/decode code can configure without Windows SDK libs.
5. Add macOS native CMake target that links vendored FFmpeg and runs a headless metadata/decode
   smoke.
6. Implement synthetic `CVPixelBuffer` texture bridge and a macOS nonblank-frame smoke path.
