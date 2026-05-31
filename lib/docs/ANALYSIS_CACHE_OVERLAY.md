# Analysis Cache And Overlay Flow

> This document covers the Flutter-side orchestration for VAC2 base cache,
> on-demand VACHUNK overlay chunks, and main-window overlay activation. Native
> file formats and analyzer internals live under `native/docs/`.
>
> The event-driven seek refresh design is tracked in
> [ANALYSIS_OVERLAY_REFRESH_DESIGN.md](ANALYSIS_OVERLAY_REFRESH_DESIGN.md).

## Ownership

| Layer | Responsibility |
| --- | --- |
| `AnalysisManager` | Compute source hash, generate/load VAC2 base, track overlay intent, submit VACHUNK requests, and reload native overlay tracks when chunks become ready. |
| `AnalysisCache` | Resolve cache paths, validate runtime VAC versions, discover current overlay chunks by file name. |
| `SerialAnalysisGenerationQueue` | Serialize VAC2 base writes and keep same-hash overlay submission behind active base writes. It is not a CPU worker pool. |
| native analysis generation service | Own the resident VACHUNK worker pool, request de-duplication, job stats, and asynchronous completion polling. |
| `MainWindowAnalysisCoordinator` | Connect active tracks, presented-frame timing, overlay panel state, analysis IPC, and redraw requests. |
| `MainWindowPlaybackCoordinator` | Notify analysis after seek settles so overlay chunks can be requested without blocking seek or frame presentation. |

Flutter never draws CU/MB geometry. It only requests cache materialization and
sends overlay mode/layer/opacity state to native. Native reads VACHUNK records
and composites the overlay during the renderer pass.

## Base Cache Generation

Opening analysis or overlay first calls:

```text
AnalysisManager.ensureGenerated(videoPath)
  -> compute SHA-256 source hash
  -> cache/<hash>/base.vac hit?
  -> SerialAnalysisGenerationQueue.generate(...)
  -> naki_analysis_generate_vac2_base(...)
```

`base.vac` is the lightweight whole-file VAC2 index. It is required before any
overlay VACHUNK can be generated. Stale VAC versions are deleted before use.
Base generation must not run the deep FFmpeg analyzer for every frame; exact
frame summaries are derived only for the overlay windows that are generated on
demand.

## Overlay Chunk Generation

Overlay chunks are generated in fixed frame windows, currently 64 frames:

```text
target presented frame
  -> current aligned 64-frame window
  -> next window at low priority during normal playback prefetch
  -> previous window too if target is in the first quarter
  -> next window too if target is in the last quarter
  -> naki_analysis_submit_vac2_overlay_chunk(...)
  -> native resident generation service
  -> naki_analysis_poll_generation_jobs(...)
```

When the target comes from the renderer-presented PTS/DTS pair, Dart schedules
the current window first and boundary windows at lower priority. This keeps the
displayed frame from waiting on adjacent prefetch work while still avoiding a
miss near 64-frame boundaries, where Dart PTS/DTS lookup and native paused-frame
lookup can differ by one frame.

Chunk requests pass through a small Dart intent scheduler so UI code can
coalesce duplicate requests and drop stale low-priority submissions under
backpressure. The resident native analysis generation service owns the real
worker pool. It de-duplicates live `(hash, startFrame, endFrame)` jobs, runs
VACHUNK generation on native worker threads, and reports completions through
`naki_analysis_poll_generation_jobs`.

VAC2 base writes remain serialized and exclusive for a hash. Overlay VACHUNK
generation may run concurrently for different frame windows once the base cache
is ready. Native worker count defaults to `min(max(2, cpu/2), 8)` and can be
overridden with `VOIDPLAYER_ANALYSIS_WORKERS`. Dart's outstanding submission
window can be adjusted with `VOIDPLAYER_ANALYSIS_SUBMISSIONS`; it is only an
intent/backpressure limit and does not perform analysis work.

While overlay is active, `MainWindowAnalysisCoordinator` also runs a low-frequency
playback prefetch tick. It samples the latest renderer-presented frame and
submits the current chunk window plus one forward window. This is intentionally
separate from the Metal/display-link hot path: playback keeps presenting,
VACHUNK generation runs in native analyzer work, and newly published chunks are
picked up by the native overlay track reload path.

Generated chunks are published under:

```text
cache/<hash>/chunks/overlay/*.vck
```

The current Dart discovery path checks the encoded `generator_revision` segment
in the file name, so old overlay chunks do not suppress regeneration after a
schema/analyzer change.

## Seek Flow

Seek does not synchronously generate analysis data:

```text
timeline / action seek
  -> NativePlayerController.seek(...)
  -> renderer presents paused preview frame
  -> native emits seekPreviewPresented(requestId, trackFileId, ptsUs, dtsUs)
  -> MainWindowPlaybackCoordinator accepts the latest requestId
  -> MainWindowAnalysisCoordinator.refreshOverlayForPresentedFrame(...)
  -> AnalysisManager schedules the needed overlay chunk window
  -> chunk completion reloads the still-requested native overlay track
  -> MainWindowController.applyLayout(...) forces redraw
```

This keeps timeline interaction responsive. The tradeoff is that overlay can
appear shortly after the video frame when the chunk was missing. A watchdog
fallback still calls `refreshOverlayForCurrentFrame()` if the native event
stream does not arrive, but the normal path is event-driven. The redraw is owned
by `onOverlayStateChanged`, not by the analyzer process.

## Presented Frame Matching

The preferred target comes from `NativePlayerController.currentPresentedFrame`
and must match both PTS and DTS through the VAC2 frame index. PTS-only matching
is avoided because damaged or unusual files can reuse PTS values. If the
presented timing is unavailable, `AnalysisManager` falls back to the native
summary current-frame estimate.

## Overlay Activation

`activateOverlayTracks()` records the requested overlay tracks immediately, sets
the current overlay mode, schedules missing chunks, and returns once the intent
is accepted. It keeps the current native overlay track binding while the same
track/hash already covers the target frame, so seek and display-link refresh do
not re-open VAC2/VACHUNK state just because the current PTS changed. Native
tracks are rebound only when the track set changes or the requested target frame
requires a newly generated chunk:

```text
AnalysisFfi.clearOverlayTracks()                                # track set changed / target missing
AnalysisFfi.setOverlayTrack(trackFileId, cache/<hash>/base.vac)  # ready chunks only
AnalysisFfi.setOverlay(...)
```

The native renderer keeps overlay tracks by file id and maps them to current
layout slots at draw time. The Dart control strip can expose one or more active
overlay track sources, while each native track manager lazily reads the relevant
VACHUNK frame on demand. The render hot path then caches per-frame primitive
packages in video coordinates; Metal separately caches the packed GPU rect/line
buffers for the same package. Layout, pan, zoom, and split changes should reuse
those cached primitives and let the compositor shader project them with the
latest layout. If a chunk is missing, the panel remains requested and native
receives the track later when the scheduler finishes that window.

## Test Guidance

- Changes to seek-triggered overlay generation should run a real timeline/seek
  path, not only direct native seek.
- Boundary regressions should use a target close to a 64-frame window edge.
- If a bug depends on missing chunks, clear `cache/<hash>/chunks/overlay/*.vck`
  while keeping `base.vac`, then rerun the UI test. UI automation can use
  `CLEAR_ANALYSIS_CHUNKS` to remove derived chunk directories without deleting
  the base cache.

Current dedicated coverage:

```text
ui_tests/analysis/overlay_seek_boundary_vvc.csv
ui_tests/analysis/overlay_seek_boundary_hevc_aq.csv
```
