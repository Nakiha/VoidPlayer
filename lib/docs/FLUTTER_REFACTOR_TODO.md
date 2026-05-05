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

## P0

- [ ] Move UI test runner/script parsing/media generation out of the production app graph behind a thin automation bridge.
- [x] ~~Add a platform service boundary so widgets do not import Win32 FFI directly.~~
- [ ] Reduce global singleton/static state, starting with injected window/process services.
- [ ] Replace viewport magic ints with a typed viewport state model that can carry user-visible errors.

## P1

- [ ] Simplify main-window coordinator dependencies by grouping state and side effects behind explicit services/stores.
- [ ] Extract a typed `NativePlayerApi` interface with DTOs and centralized MethodChannel names/payload keys.
- [ ] Move config/cache/log default paths to user-writable app data directories and make writes atomic.
- [ ] Make analysis FFI symbol lookup lazy and add a native ABI/version check.
- [x] ~~Harden analysis IPC handshake timeout and message length limits.~~

## P2

- [ ] Strengthen analyzer/linter settings once current violations are triaged.
- [ ] Split oversized main-window view/view-model surfaces by UI region.
- [ ] Make shortcut display metadata come from the same action definitions as real bindings.
- [ ] Centralize About/version/license metadata instead of hardcoding it in widgets/ARB strings.
- [ ] Extract shared layout geometry math used by production and UI test assertions.
