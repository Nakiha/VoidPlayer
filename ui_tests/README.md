# UI Test Suites

`ui_tests/` contains CSV scripts that launch the real Windows app through
`python dev.py ui-test <script>`. You can pass multiple scripts to run them in
order:

```bash
python dev.py ui-test ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
```

Pick scripts by the area touched by the change. For broad UI refactors, run a
small smoke script first, then one or more scripts from the affected folder.

## Folders

| Folder | Scope |
| --- | --- |
| `smoke/` | Fast app sanity checks. Use this for unrelated Flutter UI changes before picking a narrower regression. |
| `analysis/` | Main-window analysis spawning, analysis child-window behavior, and analysis IPC track updates. Changes under `lib/windows/analysis/`, analysis launch flow, or analysis IPC should use this folder. |
| `timeline/` | Real timeline pointer/click paths and repeated timeline seek regressions. Prefer this over direct `SEEK_TO` when a user-facing timeline interaction changed. |
| `seek/` | Direct seek, step, rapid seek, and seek crash guards. Use this when changing playback/seek logic without touching timeline pointer handling. |
| `loop/` | Loop range enable/end/handle behavior and loop frame stability. |
| `viewport/` | Window resize/maximize, viewport pan/zoom, split screen layout, and layout edge behavior. |
| `track/` | Track-level mutations such as offsets, refresh, add/remove/reorder side effects. |
| `codec/` | Codec-specific decode and non-black visual smoke checks. |
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
