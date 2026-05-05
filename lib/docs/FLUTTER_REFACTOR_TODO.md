# Flutter Refactor Todo

Source review: `build/GPT_flutter.md` (local build artifact, not tracked).

## Round 1

- [x] ~~Reset startup loop range state after the last track is removed.~~
- [x] ~~Make the startup loop range UI smoke script self-contained with app startup args.~~
- [x] ~~Add UI regression coverage for startup loop range after remove/re-add.~~

## Round 2

- [x] ~~Move ViewportPanel mouse-button recovery behind an injected platform service.~~

## Round 3

- [x] ~~Inject ActionRegistry through app/main-window/test-runner instead of using a global instance.~~

## Round 4

- [x] ~~Redact analysis IPC tokens from child process launch logs.~~

## Round 5

- [x] ~~Add analysis IPC handshake timeout and bounded JSON line parsing.~~

## Round 6

- [x] ~~Replace viewport magic ints with typed display state and render error text.~~

## Round 7

- [x] ~~Separate non-user UI automation commands from PlayerAction.~~
- [x] ~~Move TestRunner out of the actions namespace into automation.~~
- [x] ~~Route TestRunner through an explicit UiAutomationBridge.~~
- [x] ~~Extract ffmpeg test video generation out of TestRunner.~~
- [x] ~~Extract automation assertion/probe/run-state helpers out of TestRunner.~~

## Round 8

- [x] ~~Move WindowManager analysis process state behind an AnalysisProcessManager instance facade.~~
- [x] ~~Inject AnalysisProcessManager into MainWindowAnalysisCoordinator.~~
- [x] ~~Route release UI automation analysis-process access through UiAutomationBridge.~~

## Round 9

- [x] ~~Extract shared viewport display geometry math for production layout and UI automation assertions.~~
- [x] ~~Split release UI automation script model/parser out of TestRunner.~~
- [x] ~~Centralize About page version/license/dependency metadata.~~

## Round 10

- [x] ~~Move config/log/cache paths to AppData by default with exe-local cache portable mode.~~

## Round 11

- [x] ~~Add cooperative cross-process locks for config, analysis index, and per-hash cache use/generation/delete.~~
- [x] ~~Publish analysis generation from per-run staging directories instead of fixed hash tmp files.~~

## Round 12

- [x] ~~Extract native player MethodChannel method/key constants and DTO parsing out of NativePlayerController.~~
- [x] ~~Introduce NativePlayerApi and MethodChannelNativePlayerApi under NativePlayerController.~~

## Round 13

- [x] ~~Use atomic file replacement for `config.json` and `analysis_index.json` writes.~~

## Round 14

- [x] ~~Make analysis FFI symbol lookup lazy and validate native ABI metadata before use.~~

## Round 15

- [x] ~~Derive keyboard shortcut settings metadata from real `PlayerAction` definitions.~~

## Round 16

- [x] ~~Move UI automation process/window/config/media side effects behind `UiAutomationRuntime`.~~

## Round 17

- [x] ~~Move `MainWindowViewModel` and `MainWindowViewActions` out of the main view file.~~

## Round 18

- [x] ~~Enable the stricter analyzer/linter rules suggested by `build/GPT_flutter.md`.~~

## Round 19

- [x] ~~Split `MainWindowView` into scaffold, media/timeline sections, and overlay region widgets.~~

## Round 20

- [x] ~~Group `MainWindowViewModel` into viewport, media, playback, and overlay view-model slices.~~

## Round 21

- [x] ~~Group `MainWindowViewActions` into drop, toolbar, viewport, media/timeline, and overlay action slices.~~

## Round 22

- [x] ~~Route playback coordinator state access through `MainWindowStateStore` instead of a long getter/setter closure list.~~

## Round 23

- [x] ~~Route media coordinator state access through `MainWindowStateStore` instead of individual state getter/setter closures.~~

## Round 24

- [x] ~~Route layout coordinator state access through `MainWindowStateStore` and `TrackManager` instead of individual closures.~~

## P0

- [x] ~~Keep release UI automation behind explicit automation bridge/runtime boundaries.~~
  Done: non-user automation commands no longer live in `PlayerAction`; script model/parser, assertion executor, probes, run-state, video generation, process access, process exit, window automation, and automation-only config writes now sit behind the automation module/bridge/runtime instead of the action system.
- [x] ~~Add a platform service boundary so widgets do not import Win32 FFI directly.~~
- [ ] Reduce global singleton/static state, starting with injected window/process services.
  Slices done: `WindowManager` delegates analysis process state to an `AnalysisProcessManager` instance; main-window analysis and release UI automation use that instance through injection/bridge; `MainWindowController` now receives its analysis process manager and main-window fullscreen/bounds platform service through constructor injection instead of hard-reading the global facade or calling Win32/window-manager APIs directly; main-window analysis generation now depends on an injected `AnalysisGenerationService` instead of directly reading `AnalysisManager.instance`; playback seek behavior now comes from an injected `PlaybackPreferences` port instead of `MainWindowPlaybackCoordinator` reading `AppConfig.instance`; app appearance and settings pages now read/write persisted preferences through an `AppSettingsRepository` scope instead of directly using `AppConfig.instance`; cache settings open-folder behavior now goes through an injected `PathLauncher` instead of directly spawning `explorer.exe` from the widget; `ActionRegistry` now receives a `KeyboardInputService` instead of directly binding to `HardwareKeyboard.instance`.
  Compatibility note: `NEW_WINDOW` remains the documented legacy profiler-overlay action and is no longer a no-op.
- [x] ~~Replace viewport magic ints with a typed viewport state model that can carry user-visible errors.~~

## P1

- [ ] Simplify main-window coordinator dependencies by grouping state and side effects behind explicit services/stores.
  Slices done: `MainWindowPlaybackCoordinator` now depends on `MainWindowStateStore`, the timeline hover notifier, `PlaybackPreferences`, and shared `MainWindowTimelineMetrics` for playback/seek/loop state instead of a long list of individual state getter/setter closures or config singleton reads; `MainWindowMediaCoordinator` now reads/writes viewport, layout, sync offset, duration, pending seek, and audible-track state through `MainWindowStateStore`, uses `MainWindowTimelineMetrics` rather than owning effective-duration calculation for playback, and delegates cross-coordinator media lifecycle workflows to `MainWindowMediaLifecycle` instead of receiving scattered playback/controller callbacks; `MainWindowLayoutCoordinator` now reads layout/texture state through `MainWindowStateStore` and track geometry through `TrackManager`; `MainWindowController` now delegates fullscreen/bounds side effects through an injected `MainWindowPlatform`.
- [x] ~~Extract a typed `NativePlayerApi` interface with DTOs and centralized MethodChannel names/payload keys.~~
  Done: MethodChannel name/methods/keys and DTO parsing live in `native_player_protocol.dart`; concrete channel transport lives in `MethodChannelNativePlayerApi`; `NativePlayerController` owns lifecycle/no-op semantics over the typed API.
- [x] ~~Move config/cache/log default paths to user-writable app data directories and make writes atomic.~~
  Done: paths now resolve through AppData by default, with exe-local `cache/` acting as a portable-mode marker; config/index writes are guarded by companion lock files and Windows atomic replacement.
- [x] ~~Make analysis FFI symbol lookup lazy and add a native ABI/version check.~~
- [x] ~~Harden analysis IPC handshake timeout and message length limits.~~

## P2

- [x] ~~Strengthen analyzer/linter settings once current violations are triaged.~~
- [x] ~~Split oversized main-window view/view-model surfaces by UI region.~~
  Done: `MainWindowViewModel` and `MainWindowViewActions` now live outside `main_window_view.dart`; `MainWindowView` now delegates to scaffold, media/timeline section, and overlay region widgets; `MainWindowViewModel` is grouped into viewport, media, playback, and overlay slices; `MainWindowViewActions` is grouped into drop, toolbar, viewport, media/timeline, and overlay slices.
- [x] ~~Make shortcut display metadata come from the same action definitions as real bindings.~~
- [x] ~~Centralize About/version/license metadata instead of hardcoding it in widgets/ARB strings.~~
- [x] ~~Extract shared layout geometry math used by production and UI test assertions.~~
