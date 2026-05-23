# macOS Port Plan

VoidPlayer macOS support must converge on the existing native player pipeline. The macOS runner is
allowed to own Cocoa windows, MethodChannel glue, sandbox file access, and Flutter texture
registration; it must not grow a second playback backend.

Historical phase notes moved to [archive/MACOS_PORT_PLAN_HISTORY.md](archive/MACOS_PORT_PLAN_HISTORY.md).

## Current Status

- Flutter launches on macOS through a platform bootstrap with explicit capability gates; local
  native playback and native viewport capture are enabled, while network/SSH media and external
  analysis windows remain gated off.
- The macOS runner registers `video_renderer` / `video_renderer/events`, creates a
  `FlutterTexture`, and can show synthetic pixels plus local-file frames from the shared macOS
  native facade.
- macOS FFmpeg dylibs are bundled and codesigned into the debug app.
- Native macOS CMake builds portable targets: `void_player_portable_core`, `void_media_ffmpeg`,
  `void_macos_native_player`, and smoke tools.
- The visible macOS local-file path now uses `DemuxThread` + `DecodeThread` + `TrackBuffer` through
  `native/macos/native_player_bridge.*`; frames are copied into the current `CVPixelBuffer` bridge.
- H.264 playback now enables VideoToolbox through the shared hardware decode provider in
  download-to-CPU mode; diagnostics keep both the active hardware path and software fallback state
  visible.
- macOS audio now uses the shared native audio engine and miniaudio/CoreAudio output path. Automated
  diagnostics cover stream wiring and play/seek/pause/resume lifecycle; a manual audible smoke is
  documented for speaker-level confirmation.
- The native analysis library now configures and builds on macOS once the analysis submodules are
  initialized. The macOS runner links it and exports the Dart-facing `naki_analysis_*` ABI for VAC2
  base generation plus read-only handle queries. Flutter still gates unsupported macOS analysis
  windows and main-window overlays, so direct automation cannot accidentally run the Windows-only
  UI/IPC path.
- A/V sync hardening, renderer-owned Metal presentation, deeper color/layout parity, and macOS
  analysis UI/IPC support are still outstanding.

## Hard Contract

- Reuse the `NativePlayer` facade, playback semantics, demux/decode policies, seek behavior, track
  lifecycle, layout model, and capture contracts.
- Keep platform differences behind adapters: D3D11/shared-texture output on Windows,
  CVPixelBuffer/IOSurface/Metal output on macOS, and platform audio devices.
- Keep runner code as glue only. New playback state belongs in shared native components or thin
  platform adapters, not in Objective-C++ or Swift.
- Do not duplicate playback state machines, loop/timeline math, or track ownership in Objective-C++
  or Swift runner code.
- Do not introduce `libswscale` / `libyuv` as a generic fallback.

## Architecture Target

```text
Dart UI / Actions
  -> NativePlayerController
     -> platform MethodChannel adapter
        -> thin macOS/Windows runner glue
           -> shared native player facade
              -> shared demux / decode / playback / track / layout pipeline
                 -> platform output backend
                    -> D3D11 shared texture on Windows
                    -> CVPixelBuffer / IOSurface / Metal on macOS
```

## Active Milestones

### M1: Shrink The Preview Bridge

Goal: keep current macOS visible playback working while moving all reusable decode/presentation
logic out of Swift runner code.

- [x] Move FFmpeg first-frame/seek decode into `native/macos`.
- [x] Reuse shared timestamp rescaling.
- [x] Reuse shared software BGRA conversion.
- [x] Move capture/hash metrics out of Swift into a shared test/diagnostic helper or retire them
  when native capture is available.
- [x] Add native frame queue smokes for synthetic and FFmpeg-decoded software frames.
- [x] Replace timer-driven target-frame decoding with a native frame queue feeding the texture
  bridge.

### M2: Renderer-Neutral Frame Publication

Goal: split decode output from D3D11 presentation so macOS can consume frames without a second
pipeline.

- [x] Extract a platform-neutral decoded-frame publication interface from `DecodedFramePublisher`.
- [x] Split `FrameConverter` into software packing and D3D11 snapshot/presenter pieces. Software
  frame packing/wrapping is now isolated in `software_frame_packer`; D3D11VA direct-frame wrapping
  and exact-seek snapshot pooling now live behind `d3d11_frame_snapshot`; FFmpeg frame color
  metadata mapping now lives in `frame_color_metadata`. Hardware download-to-CPU, D3D11VA direct
  wrapping, and exact-seek snapshot ownership now sit behind `hardware_frame_converter`, leaving
  `FrameConverter` as the stable software entrypoint plus hardware delegate.
- [x] Keep `TrackBuffer`, `RenderSink`, seek policies, and playback clock semantics shared.
  The macOS native bridge now binds its single visible track into the shared `RenderSink`, so
  current-frame copy, frame callbacks, seek refresh, and playback ticks use the same frame-window
  evaluation semantics as the Windows renderer instead of a macOS-only clock advance loop.
- [x] Add CTest coverage that exercises software frame publication -> `TrackBuffer` without D3D11.
- [x] Extend the smoke from synthetic frames to FFmpeg-decoded frames.
- [x] Build the existing `DemuxThread` + `DecodeThread` + `TrackBuffer` software path on macOS.

### M3: Shared NativePlayer Facade On macOS

Goal: make the macOS MethodChannel call the same native player surface as Windows.

- [x] Add a macOS native library target instead of direct Xcode `#include` shims.
- [x] Define a small C/Objective-C++ bridge for create/destroy/open/play/pause/seek/currentPts.
- [x] Add a CTest-covered C ABI facade over `DemuxThread` + `DecodeThread` for macOS.
- [x] Return `available=true` only when the shared facade drives playback state.
- [x] Route the macOS MethodChannel create/play/pause/seek path through the shared facade.
- [x] Delete the legacy preview decoder target once remaining smoke coverage moves to the facade.

### M4: Software Playback MVP

Goal: local-file software playback with correct timing and basic audio.

- [x] Feed decoded frames into the macOS `CVPixelBuffer` texture bridge from a native queue.
- [x] Reuse the macOS `CVPixelBuffer` for same-sized native frame updates instead of allocating a
  new pixel buffer per frame.
- [x] Copy native playback frames directly into the locked `CVPixelBuffer` BGRA memory during
  frame-callback playback, avoiding the intermediate native BGRA allocation and Swift `Data` copy.
- [x] Add CoreAudio/miniaudio output behind the existing audio abstraction.
- [x] Validate audio play/seek/pause/resume and destroy/recreate lifecycle through macOS UI smokes.
- [x] Route loop range to the macOS native facade and cover it with CTest/UI smoke.
- [x] Move macOS loop enforcement out of the visible frame-copy path and into a native playback
  tick.
- [x] Exercise explicit test shutdown while playback is active.
- [x] Validate user main-window close while native playback is active.
- [x] Validate inactive audible-track PCM behavior in shared `AudioMixer`.
- [x] Add user-observable audible playback notes or tooling.
- [ ] Preserve Windows behavior and tests.

### M5: Metal And Hardware Decode

Goal: improve performance after software playback is correct.

- [ ] Add a Metal/CVPixelBuffer backend with the same responsibilities as D3D11 output.
  The boundary now lives in `native/macos/presentation_adapter.*`; see
  [MACOS_PRESENTATION_ADAPTER.md](MACOS_PRESENTATION_ADAPTER.md).
  The runner now creates Metal-compatible `CVPixelBuffer` surfaces, while
  `metal_pixel_buffer_uploader.mm` owns `CVMetalTextureCache` validation, the shared `MTLBuffer`,
  and the blit into the texture-backed `CVPixelBuffer`. Seek/step refresh and playback callbacks
  now share that native Metal staging path; the locked-buffer direct copy path remains as fallback.
- [ ] Port shader/color/layout behavior with deterministic pixel tests.
  Initial portable baselines now cover limited/full-range software BGRA conversion, padded
  linesizes, BGRA channel order, BT.601/BT.709/BT.2020 matrix selection in the shared CPU
  YUV-to-BGRA helper, BT.2020 and unknown-HD-to-BT.709 matrix selection in the macOS presentation
  adapter, odd-dimension NV21 -> even-coded NV12 packing, planar YUV420 wrap metadata, and P010 ->
  BGRA presentation conversion.
- [x] Add VideoToolbox behind the hardware decode provider interface.
  The provider is registered through the shared `HwDecodeProvider` factory and now runs the macOS
  facade through `FfmpegOwnedHwDownloadDevice` by default. This keeps decoded frames published
  through the existing CPU frame path instead of creating a second renderer backend.
- [x] Keep software fallback visible in diagnostics.

### M6: Packaging And Release

Goal: make the app distributable and honest about unsupported features.

- [x] Resolve the FFmpeg dylib deployment-target mismatch by declaring macOS 14.0 as the current
  app minimum and passing the same deployment target into the Xcode-triggered native CMake build.
- [x] Add release build, signing, notarization, and third-party notice docs.
  `dev.py package` now stages a macOS release app with bundled GPL/third-party/FFmpeg notices and
  verifies bundled `@rpath` linkage before ad-hoc signing/verifying the copied app signature.
  It can also Developer ID sign the staged app when `--macos-sign-identity` or
  `VOIDPLAYER_MACOS_SIGN_IDENTITY` is provided. `dev.py package --installer` creates a local
  compressed DMG and can submit/staple/validate it through `xcrun notarytool` when
  `--macos-notarize --macos-notary-profile` or `VOIDPLAYER_MACOS_NOTARY_PROFILE` is provided.
  Local staging and DMG creation passed on 2026-05-23; real release credentials remain an
  operator-supplied step.
- [x] Make the native analysis library build on macOS with the initialized submodules.
  CI now has a macOS analysis job that configures `BUILD_ANALYSIS=ON`, builds the app-facing
  analysis FFI smoke, and runs VAC2 base generation plus handle readback on a bundled H.264 sample.
- [x] Add an explicit unsupported gate for macOS analysis windows and overlays until the workflow is
  wired.
- [ ] Decide macOS analysis UI/IPC support: native library, helper process, or a first-class
  in-process analysis workspace. The native-library path now has a first foothold: macOS exports
  the shared Dart analysis ABI, while overlay VACHUNK generation intentionally returns unsupported
  until the analyzer/helper process contract is ported.
- [x] Add macOS native CTest CI for the shared facade/presentation/hardware-decode smoke suite.
  Flutter macOS UI automation remains local-only until a reliable headed CI strategy is chosen.

## Validation Matrix

| Change | Minimum validation |
| --- | --- |
| Portable native code | `cmake -S native -B native/build-macos-make -G "Unix Makefiles" ...` and `ctest --test-dir native/build-macos-make --output-on-failure` |
| macOS runner/texture | `flutter build macos --debug` and relevant `python dev.py mac-ui-test ...` scripts |
| Dart platform/UI | `flutter analyze` plus targeted Flutter/unit/UI tests |
| Shared native playback behavior | Windows `python dev.py test --native-only` plus macOS portable CTest |
| Packaging | `codesign --verify --deep --strict` and `otool -L` on app artifacts |

Current macOS smoke set:

```bash
python dev.py mac-ui-test \
  ui_tests/macos/synthetic_texture_smoke.csv \
  ui_tests/macos/native_facade_smoke.csv \
  ui_tests/macos/native_first_frame_smoke.csv \
  ui_tests/macos/native_controls_smoke.csv \
  ui_tests/macos/native_seek_frame_smoke.csv \
  ui_tests/macos/native_playback_smoke.csv \
  ui_tests/macos/native_frame_callback_lifecycle_smoke.csv \
  ui_tests/macos/native_callback_stress_smoke.csv \
  ui_tests/macos/native_playing_seek_keeps_state_smoke.csv \
  ui_tests/macos/native_playing_step_pauses_smoke.csv \
  ui_tests/macos/native_loop_range_smoke.csv \
  ui_tests/macos/native_audio_diagnostics_smoke.csv \
  ui_tests/macos/native_audio_play_seek_smoke.csv \
  ui_tests/macos/native_audio_destroy_recreate_smoke.csv \
  ui_tests/macos/native_quit_while_playing_smoke.csv \
  ui_tests/macos/native_user_window_close_smoke.csv \
  ui_tests/macos/native_direct_copy_fallback_smoke.csv \
  ui_tests/macos/native_p010_presentation_smoke.csv \
  ui_tests/macos/native_software_fallback_smoke.csv \
  ui_tests/macos/analysis_gated_smoke.csv
```

The full macOS smoke set, including `native_callback_stress_smoke.csv`, passed on 2026-05-23 after
the D3D11 frame snapshot split and macOS DMG packaging changes. The same set passed with `--build`
earlier on 2026-05-23 against the rebuilt Debug app. An earlier full-set run exposed that
`native_playing_seek_keeps_state_smoke.csv` used a fixed-time `ASSERT_PLAYING` that was too tight
under batch load; the script now waits for playing state before checking post-seek position and
presented-frame ranges. On 2026-05-22, targeted loop/audio/quit validation passed with:

```bash
python dev.py mac-ui-test --build \
  ui_tests/macos/native_loop_range_smoke.csv \
  ui_tests/macos/native_audio_play_seek_smoke.csv \
  ui_tests/macos/native_quit_while_playing_smoke.csv
```

Known build warning: the local Xcode/Flutter invocation may still report a missing transient Metal
toolchain Swift search path under `/var/run/com.apple.security.cryptexd`; the FFmpeg dylib
deployment target is now aligned to macOS 14.0.

`CAPTURE_VIEWPORT` hash/luma/non-black metrics now use the shared native
`bgra_capture_metrics` helper. CTest covers the helper through `bgra_capture_metrics_smoke`, and
macOS UI smoke covers both synthetic and native first-frame capture payloads.

Current loop status: macOS loop enforcement now runs from a native playback tick owned by the
macOS native player bridge. The same native tick emits frame-available callbacks when playback
advances the current frame; Swift copies that frame into the `CVPixelBuffer` texture and no longer
owns a fixed playback timer or loop decisions.

Analysis status: `native/build-macos-analysis` now configures with `BUILD_ANALYSIS=ON` and builds
`analysis_lib` on macOS. The portability fixes live in the shared UTF-8 filesystem/env shim, so the
same analysis/cache code can compile without Windows-only file helpers. The macOS runner now links
that library through the native player archive, force-loads the app-facing bridge so Dart FFI can
resolve `naki_analysis_*`, and `macos_analysis_ffi_smoke` verifies VAC2 generation plus handle
readback. This does not yet mean the macOS app can launch or coordinate analysis windows;
`PlatformCapabilities.macOSPhase1` keeps external analysis windows and main-window overlays
disabled, and `analysis_gated_smoke.csv` asserts that the FFI is present, generates and loads a
VAC2 base through `AnalysisManager`, and still confirms that direct analysis window/overlay
automation no-ops instead of spawning the Windows-only UI/IPC path.

Publication status: decoded-frame publication now flows through a `DecodedFrameSink` interface. The
default sink preserves the existing `TrackBuffer` behavior, and `decoded_frame_sink_smoke` covers
frame delivery plus conversion-failure error state. macOS presentation now has a native
`presentation_adapter` boundary that copies shared `TextureFrame` storage into a caller-provided
BGRA destination; Swift owns `CVPixelBuffer` lifecycle, locking, and Flutter texture notification
only. This prepares the decode thread for a future Metal/CVPixelBuffer sink without creating
another decode backend.

Runner cleanup status: the obsolete `MacOSFirstFrameDecoder` ObjC++ shim has been removed; the
Swift bridging header now includes the macOS native player bridge directly.

Frame conversion split status: deterministic software YUV/NV12/P010 packing and planar YUV420
wrapping now live in `video_renderer/decode/software_frame_packer.*`. D3D11VA direct-frame wrapping
and exact-seek snapshot pooling now live in `video_renderer/decode/d3d11_frame_snapshot.*`, while
hardware download-to-CPU and platform-owned frame publication are delegated through
`video_renderer/decode/hardware_frame_converter.*`. This keeps the current macOS playback queue on
the shared native decode path without starting a second backend. The macOS native bridge also routes
visible-frame selection through `RenderSink::evaluate()`, preserving shared `TrackBuffer` advancement
and clock-window rules for first frame, seek refresh, and playback frame callbacks.

M5 baseline status: `software_bgra_converter_smoke` now includes deterministic limited/full-range
color samples, padded line strides, and BT.709 matrix conversion, while `software_frame_packer_smoke`
locks odd-size NV21 packing and planar YUV420 wrap metadata. `macos_presentation_adapter_smoke`
covers the macOS presentation boundary directly, including CPU RGBA stride copies, CPU NV12/P010
color conversion, BT.709/BT.2020 matrix-aware presentation, unknown HD matrix fallback to BT.709,
planar YUV conversion, adapter identity, and undersized P010 rejection. These are CPU-side reference
points for future Metal and CVPixelBuffer layout parity tests. `videotoolbox_provider_smoke`
now proves that the macOS FFmpeg build can initialize the shared VideoToolbox provider for H.264 in
download-to-CPU mode. UI automation can assert string-valued native diagnostics through
`ASSERT_NATIVE_DIAGNOSTIC_STRING`, and macOS facade/stress smokes now lock
`presentationAdapter=cvpixelbuffer-bgra-copy` as the visible software presentation fallback before
deeper Metal color work starts. UI automation can also assert boolean diagnostics through
`ASSERT_NATIVE_DIAGNOSTIC_BOOL`. The macOS runner reports Metal surface readiness through
`metalAvailable`, `metalTextureCacheAvailable`, `metalTextureValid`, and
`metalTextureCreationCount`; facade/stress smokes assert that Metal wrapping is valid, that
`presentationUploadMode=metal-bgra-staging-upload`, and that playback produces at least one native
Metal staging upload for the active pixel buffer. `native_direct_copy_fallback_smoke.csv` disables
the test-only Metal upload path and asserts the same native frames can still present through
`presentationUploadMode=cvpixelbuffer-direct-copy` with visible pixels. The facade smoke also reports
`hardwareDecodeProvider=VideoToolbox`, `hardwareDecodeAvailable=true`,
`hardwareDecodeActive=true`, `hardwareDecodeDownloadsToCpu=true`,
`decodeMode=videotoolbox-download-to-cpu`, and `softwareFallbackActive=false` for the H.264 sample,
keeping the fallback contract explicit when VideoToolbox is unavailable or unsupported.
`native_software_fallback_smoke.csv` covers a generated MPEG-2 path and asserts
`decodeMode=software-fallback`, `hardwareDecodeActive=false`, and
`softwareFallbackActive=true`. `native_p010_presentation_smoke.csv` covers generated 10-bit H.264
VideoToolbox download-to-CPU decode through the P010 presentation path with the direct-copy fallback
enabled.

Packaging status: `python dev.py package` now works on macOS as a release staging command. It builds
or reuses `build/macos/Build/Products/Release/VoidPlayer.app`, copies it to
`build/package/macos/VoidPlayer/VoidPlayer.app`, adds top-level and in-app compliance docs, stages
the macOS FFmpeg README/build manifest/license files, runs the release compliance smoke, verifies
bundled `@rpath` linkage with `otool -L`, ad-hoc signs the staged copy, and verifies the app with
`codesign --verify --deep --strict`. Passing `--installer` creates
`build/package/macos/installer/VoidPlayer-<version>-macos-arm64.dmg` with `hdiutil` for local
testing. Passing `--macos-sign-identity` replaces ad-hoc signing with Developer ID hardened-runtime
signing, and `--macos-notarize --macos-notary-profile` notarizes, staples, and validates the DMG.
Local validation passed on 2026-05-23 with `python3 dev.py package --no-build` and
`python3 dev.py package --no-build --installer`.

Frame callback lifecycle status: macOS now has targeted UI smokes that churn play/pause/play,
play/seek/pause, destroy/recreate, and pixel-buffer reuse diagnostics while native frame callbacks
are active. `native_callback_stress_smoke.csv` adds rapid play/pause, a playing seek storm,
play-then-destroy, recreate, and play-then-main-window-close coverage. The smokes also verify
`pixelBufferMetalUploadCount`, so the native-owned Metal staging upload path is covered by UI
automation. `pixelBufferDirectCopyCount` remains diagnostic-only for fallback visibility.
Main-window close while playing remains covered independently by `native_user_window_close_smoke.csv`.

Windows preservation status: on this macOS host, `python dev.py test --native-only` now runs the
macOS portable CMake/CTest suite instead of preparing the Windows analyzer. The Windows analyzer
builder has been folded into Python under `dev.py`; run the native-only suite and a Windows UI
smoke that exercises `QUIT` after player creation on a Windows host before closing M4.

CI status: `.github/workflows/native.yml` now has a `macos-14` native job that checks out Git LFS
FFmpeg artifacts and runs `python dev.py test --native-only`, covering the portable macOS CTest
suite without introducing headed Flutter UI automation into CI yet. A second macOS job configures
`BUILD_ANALYSIS=ON`, builds `macos_analysis_ffi_smoke`, and runs VAC2 generation/readback through
the same `naki_analysis_*` ABI that Dart uses. A third macOS job runs `flutter build macos --debug`,
covering Swift runner, Xcode project, the analysis-enabled native static library, and FFmpeg dylib
linkage. The native test job sets `VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1` because GitHub macOS runners
can report VideoToolbox availability but fail real H.264 hardware decode initialization; local
UI/facade smoke remains the hardware decode validation point.

Manual audible smoke:

```bash
python dev.py mac-ui-test --visible --build ui_tests/macos/native_audio_play_seek_smoke.csv
```

Listen for the generated sine tone before and after play-time seek, pause, and resume. This fills
the speaker-level gap that the current native diagnostics cannot observe directly.

## M4 Exit Criteria

- Full macOS smoke set passes with `--build`.
- Portable macOS CTest passes for the shared native facade/audio path.
- Manual audible smoke confirms speaker output on the default macOS output device.
- Windows native-only and one Windows UI smoke with player creation plus `QUIT` pass on a Windows
  host.
- Loop-range enforcement remains native-tick driven and does not regress to texture frame-copy
  coupling.

## Next Slice

The next implementation slice should continue the M5 Metal/CVPixelBuffer backend or start the
first-class macOS analysis workflow behind the explicit capability gates, while keeping the Windows
native/UI preservation checks on the first available Windows host.
