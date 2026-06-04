# Analysis Cache

This document describes the current cache architecture for progressive
analysis. Runtime cache writes VAC2 base files plus VACHUNK derived chunks.

## Goals

- Open the analysis window after a lightweight whole-file pass.
- Keep playback on the hardware decode path.
- Make NAL unit details, exact frame summaries, and overlays on-demand.
- Cache derived results by range and feature set, then discard them safely.
- Preserve useful immediate views: NALU list, packet/frame size charts,
  reference pyramid, coarse QP trend, and distribution charts.
- Make CLI/agent analysis deterministic: it can read base data, request missing
  chunks, or report which derived capabilities are absent.
- Use the same FFmpeg-based on-demand analyzer contract for codec-specific
  derived analysis.

## Non-Goals

- VAC2 does not need to store every codec syntax field in the base file.
- VAC2 does not make CU/PU/TU overlay data mandatory.
- VAC2 does not require one monolithic appendable file for every derived result.
- VAC2 does not promise exact QP heatmaps without running a derived analyzer.

## Cache Layout

Runtime cache uses one directory per source hash:

```text
cache/
  <hash>/
    base.vac
    meta.json
    locks/
    chunks/
      frame_summary_exact/
        vvc_qp_refs_00001234_00001289.vck
      overlay/
        vvc_cu_qp_mv_00001234_00001289.vck
      nalu_detail/
        00004567.vck
    tmp/
```

`base.vac` is the VAC2 base index. It is immutable for a given source hash and
generator version. Files under `chunks/` are derived analysis chunks. They are
safe to delete and regenerate.

`meta.json` is a management index, not the source of truth for binary parsing.
It records chunk state, byte sizes, generator versions, access timestamps, and
incomplete temp cleanup hints.

## Two Artifact Classes

### Base Index

The base index is the "map" of the bitstream. It should be small enough to build
quickly with demux and syntax-header parsing, without full frame reconstruction.

It owns:

- container/track metadata
- packet timing and file/format offsets
- bitstream unit records
- access-unit/frame mapping
- parameter-set snapshots
- lightweight frame summaries
- coarse timeline buckets and histograms

The base index must be enough to seek source data for derived analysis. A derived
analyzer should be able to ask: "for frame N, where is the nearest random access
point, which packets and parameter sets do I need, and which source byte ranges
should be read?"

### Derived Chunks

Derived chunks store expensive results:

- exact frame QP/reference summaries
- NAL unit detail trees
- CU/MB/PU/TU records
- overlay-ready geometry and scalar streams
- hit-test spatial indexes
- export-only advanced syntax summaries

Chunks are generated on demand. They are range-scoped and feature-scoped, so a
user looking at three frames does not force a full-file decode.

## Base Views

The following views should work from VAC2 base data alone:

| View | Required Base Data | Notes |
| --- | --- | --- |
| NALU list | bitstream unit table | Includes type, flags, offsets, packet/AU mapping. |
| Packet/frame size chart | packet/frame summary | Exact packet sizes are demux-level data. |
| NALU distribution | bitstream unit table/buckets | Can be pre-bucketed for large files. |
| Reference pyramid | frame summary refs | Requires parsed slice/header reference info, not CU data. |
| Coarse QP trend | frame summary QP | Uses slice/base QP until exact QP chunk exists. |
| Basic CLI summary | all above | Deterministic and fast. |

QP values in the base index must carry a quality marker:

- `unknown`: no QP was parsed.
- `slice`: direct slice/header QP.
- `base`: frame-level initial/base QP derived from parameter sets and slice
  header state.
- `estimated`: a cheap approximation.
- `exact`: CU/MB-derived aggregate from a derived analyzer.

The UI can draw a QP trend from non-exact values, but exact QP heatmaps remain a
derived capability.

## On-Demand Analyzer Contract

VAC2 enables local decode/parse jobs. A job starts from a requested capability,
range, and track:

```text
request: track, feature_set, target_frame_or_nalu, optional window
VAC2:    locate RAP/GOP, packets, parameter sets, source byte ranges
job:     seek/read/decode/parse only the required region
output:  validated VACHUNK file published under chunks/<kind>/
UI:      pending -> ready, then repaint or refresh detail view
```

Frame-oriented jobs should use random access points or GOP boundaries as their
natural range. A chunk may cover more frames than requested if that improves
reuse.

The current runtime overlay implementation publishes validated `.vck` files
directly under `cache/<hash>/chunks/overlay/`; file names encode the chunk kind,
feature mask, base revision, generator revision, and covered frame range. Dart
uses those names plus the VACHUNK header validation path rather than treating
`meta.json` as an authoritative chunk registry.

NAL-detail jobs can be single-unit chunks. They should parse the selected NAL
using the parameter-set snapshot active at that point.

## Analyzer Contract

- The vendored FFmpeg analyzer emits VACHUNK overlay data directly for supported codecs.
- Dart/native FFI requests bounded overlay windows instead of full-file deep artifacts.
- Codec-specific hooks live in the FFmpeg analyzer; runtime cache consumers read only validated VAC2/VACHUNK files.

## Concurrency And Locks

- A base index has one shared read lock and one exclusive generation lock.
- Each chunk has its own generation lock.
- Chunks are written under `tmp/`, validated, then atomically published.
- Readers must ignore temp files and must validate VACHUNK header kind/range,
  base revision, and generator revision. Runtime overlay discovery currently
  uses published `.vck` file names plus header validation.
- Deleting chunks must not invalidate `base.vac`.
- Regenerating `base.vac` for a different source/generator version invalidates
  all chunks under that hash directory.

## Chunk Granularity

Default frame-oriented chunk boundaries:

- start at a 64-frame aligned window
- end at the inclusive end of that window or the final frame
- never split one decoded frame across chunks
- near the first/last quarter of a window, request the adjacent window too so
  seek-to-boundary redraws cover the actual presented frame
- prefer reuse over exact request size, but keep chunks small enough for
  interactive cache pruning

Recommended initial policy:

- NAL detail: one NAL or small adjacent range.
- Exact frame summary: GOP/window chunk.
- Overlay CU/QP/MV/bit density: aligned 64-frame window chunk.
- Export batch: may request larger sequential chunks, but still writes normal
  chunk files.

## Verification

Required test families:

- VAC2 binary parser/writer format tests.
- Base index generation tests for MP4/FLV/raw Annex-B samples.
- Source mapping tests: frame/NALU -> packet/range/RAP.
- Chunk validation and corruption recovery tests.
- UI tests proving analysis opens from base-only cache.
- Overlay tests proving missing chunks enter pending state and ready chunks
  change viewport pixels.
- CLI/export tests for base-only, chunk-missing, and chunk-ready states.
