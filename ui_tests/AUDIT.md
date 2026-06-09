# UI Test Audit

This audit is a cleanup planning document for `ui_tests/`. It is not a deletion
list. Confirm lower-level coverage or a broader UI script before removing any
CSV.

## Inventory

- Total CSV scripts: 132
- Total CSV lines: 2404

| Folder | Count | Primary coverage | Cleanup posture |
| --- | ---: | --- | --- |
| `macos/` | 36 | macOS app bundle, FlutterTexture/Metal/VideoToolbox, audio, and window lifecycle visible-path smokes. | Keep separate from Windows; trim nightly/default sets before deleting scripts. |
| `analysis/` | 29 | Analysis process spawn, child analysis window scripts, overlay controls, IPC, and track sync. | Keep representative spawn/overlay/IPC scripts; consolidate codec matrix where parent/child pairs duplicate. |
| `timeline/` | 16 | Real timeline pointer/click seek paths, click storms, and resource regressions. | Keep real click-path tests; move pure seek/resource variants toward seek/native tests. |
| `seek/` | 15 | Direct seek, step, rapid seek, and seek crash guards. | Keep rapid/crash/native timing cases; push pure coordinator behavior to Dart tests. |
| `viewport/` | 8 | Resize/maximize, pan/zoom, split layout, and edge clipping. | Mostly valid UI-layer coverage; keep, avoid adding state-only regressions. |
| `local/` | 7 | Machine-local private media regressions. | Keep isolated; never default-gate. Consider documenting private media dependencies. |
| `smoke/` | 7 | Fast broad sanity checks. | Keep small; avoid turning smoke into a regression dump. |
| `track/` | 7 | Add/remove/reorder/offset track mutations. | Mostly valid integration coverage; merge adjacent remove/readd cases only if runtime hurts. |
| `codec/` | 3 | Codec-specific decode/non-black smoke. | Keep only codecs that have distinct backend coverage. |
| `loop/` | 3 | Loop range behavior and loop frame stability. | Keep handle/visual cases; pure loop math belongs in unit/native tests. |
| `color/` | 1 | Portable visual-diff/color pipeline checks. | Keep as specialized visual/color check, not smoke. |

## Why macOS Has Its Own Command

`python dev.py mac-ui-test` is not just `ui-test` with another executable. It is
macOS-specific because it:

- launches the `.app` through `/usr/bin/open -W`, matching app-bundle behavior
  instead of running a bare executable;
- copies CSV scripts and repo media fixtures into the sandbox container under
  `~/Library/Containers/dev.nakiha.voidplayer/Data/tmp/voidplayer-ui-tests`;
- rewrites `ADD_MEDIA` and generated-media rows so sandboxed debug builds can
  open fixtures;
- re-installs helper dylibs/tools, re-signs the app bundle, and registers the
  bundle before launch;
- scans macOS crash reports and Flutter AXTree errors around the app-bundle run.

The separate `ui_tests/macos/` folder is justified for app-bundle, sandbox,
Metal/FlutterTexture, VideoToolbox, audio, and macOS window lifecycle risks.
Cross-platform UI behavior should not automatically get a macOS-only CSV unless
one of those risks is involved.

## Duplicate / Matrix Families

These groups are likely intentional matrices, but should be reviewed before
adding siblings:

- `analysis/child_<codec>_codec` (4 scripts): `child_h264_codec.csv`,
  `child_h265_codec.csv`, `child_mpeg2_codec.csv`, `child_vp9_codec.csv`
- `analysis/overlay_controls_<codec>` (3 scripts): `overlay_controls_h264.csv`,
  `overlay_controls_h265.csv`, `overlay_controls_vvc.csv`
- `analysis/spawn_<codec>` (5 scripts): `spawn_h264.csv`, `spawn_h265.csv`,
  `spawn_mpeg2.csv`, `spawn_vp9.csv`, `spawn_vvc.csv`
- `codec/<codec>_not_black_regression` (2 scripts):
  `av1_not_black_regression.csv`, `vp9_not_black_regression.csv`
- `timeline/<codec>_timeline_exact_seek_resource_regression` (5 scripts):
  `av1_timeline_exact_seek_resource_regression.csv`,
  `h264_timeline_exact_seek_resource_regression.csv`,
  `h265_timeline_exact_seek_resource_regression.csv`,
  `h266_timeline_exact_seek_resource_regression.csv`,
  `vp9_timeline_exact_seek_resource_regression.csv`

## Cleanup Candidates

Do not delete these blindly; first confirm whether the risk is now covered by
lower-level tests or a broader script.

| Candidate | Reason to review | Suggested action |
| --- | --- | --- |
| `timeline/*_exact_seek_resource_regression.csv` codec matrix | Five codec variants mostly assert seek/resource stability with similar structure. | Keep one smoke/resource representative in default paths; move codec-specific resource logic to native/backend tests where possible. |
| `analysis/spawn_*` + `analysis/child_*_codec.csv` codec matrix | Parent/child codec pairs may duplicate analysis decode capability checks. | Keep representative parent scripts; use native/analysis tests for codec parsing/count details. |
| `seek/h265_*visual_regression.csv` cluster | Many H265 visual step/seek cases overlap with macOS step/seek and timeline visual scripts. | Keep cases that prove distinct UI path; move exact frame/PTS math to native/Dart tests. |
| `macos/native_*_smoke.csv` nightly set | Many are valid platform smokes, but not all should be run for ordinary Flutter changes. | Keep scripts; tighten gate membership and selection docs before deleting. |
| `local/` | Non-portable private-media regressions. | Keep isolated; never cite as required validation unless user/developer explicitly asks. |

## Policy

- Do not add a UI CSV for pure Dart state, view-model filtering, coordinator
  branch behavior, persistence, or mark/list selection bugs; add `test/unit/`
  coverage instead.
- Add or keep UI CSV only for real input paths, native texture/presentation,
  app-bundle/window integration, cross Flutter/native timing, visual/hash
  assertions, codec/hardware backend, or multi-process boundaries.
- Prefer extending an existing folder script over adding a sibling when the new
  scenario is only a parameter or codec variant.
- Treat stress/resource/visual/hash scripts as targeted or nightly validation,
  not default smoke.
