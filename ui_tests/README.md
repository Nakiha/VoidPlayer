# UI Test Suites

`ui_tests/` contains CSV scripts that launch the real app. Windows uses
`python dev.py ui-test <script>`, while macOS uses
`python dev.py mac-ui-test <script>` so sandboxed debug builds can read copied
fixtures from the app container. You can pass multiple scripts to run them in
order:

```bash
python dev.py ui-test ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
```

Pick scripts by the area touched by the change. For broad UI refactors, run a
small smoke script first, then one or more scripts from the affected folder.

## Commands

- `python dev.py ui-test ...` launches the Windows runner directly with the CSV
  script path.
- `python dev.py mac-ui-test ...` launches the macOS `.app` bundle through
  `/usr/bin/open`, copies scripts/media into the app sandbox, rewrites
  `ADD_MEDIA` fixture paths, re-signs/registers the bundle, and scans macOS
  crash reports. Use it for `ui_tests/macos/` and for macOS app-bundle,
  sandbox, native target ring/Metal composition, VideoToolbox, audio, or window
  lifecycle risks.

## Governance

UI CSV scripts launch the real app and should stay focused on risks that cannot
be covered cheaply below this layer: native texture presentation, runner/window
integration, platform input sequences, cross Flutter/native timing, visual/hash
assertions, codec backends, and end-to-end process boundaries.

Before adding a new script, prefer these cheaper options:

- Add or update a Dart unit/widget test for Flutter state, view models,
  coordinator branches, filtering/sorting, persistence, shortcuts, focus, and
  hit testing.
- Add or update a native unit test for deterministic renderer, seek/clock,
  layout, parser, cache/index, or conversion logic.
- Extend an existing script in the same folder when the new case is only a
  parameter variant of an existing smoke/regression.

Use new CSV files for distinct user-visible workflows or platform/backend
regressions. Keep stress/resource/visual/hash scripts out of default smoke
paths unless the current change specifically touches that risk.

## Folders

| Folder | Scope |
| --- | --- |
| `macos/` | macOS runner, native target ring, Metal compositor, VideoToolbox, and shared native facade smokes. |
| `smoke/` | Fast app sanity checks. Use this for unrelated Flutter UI changes before picking a narrower regression. |
| `analysis/` | Main-window analysis spawning, analysis child-window behavior, and analysis IPC track updates. Changes under `lib/windows/analysis/`, analysis launch flow, or analysis IPC should use this folder. |
| `timeline/` | Real timeline pointer/click paths and repeated timeline seek regressions. Prefer this over direct `SEEK_TO` when a user-facing timeline interaction changed. |
| `seek/` | Direct seek, step, rapid seek, and seek crash guards. Use this when changing playback/seek logic without touching timeline pointer handling. |
| `loop/` | Loop range enable/end/handle behavior and loop frame stability. |
| `viewport/` | Window resize/maximize, viewport pan/zoom, split screen layout, and layout edge behavior. |
| `track/` | Track-level mutations such as offsets, refresh, add/remove/reorder side effects. |
| `codec/` | Codec-specific decode and non-black visual smoke checks. |
| `color/` | Portable software/hardware decode visual-diff and color pipeline checks. |
| `local/` | Machine-local regressions that depend on absolute paths or large private videos. Do not treat these as portable default checks. |

## Analysis Tests

The analysis folder has two kinds of scripts:

- `spawn_*.csv` and `ipc_*.csv` run from the main window. They generate analysis,
  spawn or reuse the analysis workspace process, and may pass a child script to
  the spawned analysis window.
- `child_*.csv` run inside an analysis child/standalone window. They are support
  scripts for the main-window spawn scripts, not normal main-window UI tests.

For analysis-window refactors, use the `analysis/` folder rather than
`smoke/basic.csv` alone.

## Naming

Use `<area>/<scenario>_regression.csv` for regressions and
`<area>/<scenario>_smoke.csv` for broad sanity checks. Keep child/helper scripts
near the parent scripts that reference them.
