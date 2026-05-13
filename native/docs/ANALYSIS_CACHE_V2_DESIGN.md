# Analysis Cache V2 Design

This document describes the cache architecture for progressive analysis. The
runtime cache now writes VAC2 base files plus VACHUNK derived chunks; the old
VAC1 container remains only as migration history in this design note.

## Problem

The VAC1 pipeline treats analysis as a complete offline artifact:

1. Scan or demux the whole file.
2. Run codec-specific deep analyzers before the UI can use the result.
3. Write every analysis payload into one VAC file.
4. Allow analysis UI and overlay only after the whole artifact exists.

That model blocks lightweight workflows. A user who only wants a few NAL unit
details, a packet-size chart, a reference pyramid, or a short overlay window
still pays the cost of deep analysis over the entire file.

The target architecture separates always-useful base indexing from expensive
derived analysis.

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

The cache moves from one file per hash to one directory per hash:

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

The base index must be enough to seek source data for future analysis. A derived
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
output:  VACHUNK file + meta.json registration
UI:      pending -> ready, then repaint or refresh detail view
```

Frame-oriented jobs should use random access points or GOP boundaries as their
natural range. A chunk may cover more frames than requested if that improves
reuse.

NAL-detail jobs can be single-unit chunks. They should parse the selected NAL
using the parameter-set snapshot active at that point.

## External Analyzer Retirement

The old external decoder flow was useful for historical full-file block-stat
analysis, but it was a poor fit for progressive analysis because it was not
built around seeking and partial decode jobs.

Current direction:

1. Use the vendored FFmpeg analyzer to emit VACHUNK overlay data directly for
   supported codecs.
2. Use VAC2 source mapping to seek to random access points and decode only the
   requested range.
3. Extend the FFmpeg analyzer contract when VVC/H.266 overlay coverage is
   ready, instead of reintroducing full-file decoder artifacts.

## Concurrency And Locks

- A base index has one shared read lock and one exclusive generation lock.
- Each chunk has its own generation lock.
- Chunks are written under `tmp/`, validated, then atomically published.
- Readers must ignore chunks not registered as complete in `meta.json`.
- Deleting chunks must not invalidate `base.vac`.
- Regenerating `base.vac` for a different source/generator version invalidates
  all chunks under that hash directory.

## Chunk Granularity

Default frame-oriented chunk boundaries:

- start at a random access point or GOP start
- end at GOP end or a bounded UI window
- never split one decoded frame across chunks
- prefer reuse over exact request size, but keep chunks small enough for
  interactive cache pruning

Recommended initial policy:

- NAL detail: one NAL or small adjacent range.
- Exact frame summary: GOP/window chunk.
- Overlay CU/QP/MV: GOP/window chunk.
- Export batch: may request larger sequential chunks, but still writes normal
  chunk files.

## Migration Plan

### Phase 1: Base-Only Cache

- [x] Add VAC2 writer/reader.
- [x] Generate packet, bitstream-unit, AU/frame, parameter-set, and lightweight
  frame-summary sections.
- [x] Allow analysis UI to open H.264/HEVC/VVC when only base data exists.
- [x] Remove VAC1 as a runtime cache write path.

### Phase 2: NAL Detail On Demand

- Add native APIs to request and read NAL detail rows.
- Cache detail chunks under `chunks/nalu_detail/`.
- Use VAC2 parameter-set snapshots and source offsets.

### Phase 3: Frame Summary Refinement

- Add exact frame-summary chunks.
- Let charts upgrade from base QP/ref quality to exact quality when chunks are
  available.
- Keep base reference pyramid usable without exact chunks.

### Phase 4: Overlay Chunks

- [x] Add request/ready APIs for overlay features.
- [x] Generate CU/MB/QP/MV overlay chunks for bounded frame windows.
- [x] Gate and load main-window overlays from VACache chunks.

### Phase 5: FFmpeg VVC Analyzer

- [x] Extend the FFmpeg-based analyzer contract to VVC/H.266.
- [x] Generate VVC overlay chunks through the FFmpeg analyzer path.

## Testing

Required test families:

- VAC2 binary parser/writer format tests.
- Base index generation tests for MP4/FLV/raw Annex-B samples.
- Source mapping tests: frame/NALU -> packet/range/RAP.
- Chunk validation and corruption recovery tests.
- UI tests proving analysis opens from base-only cache.
- Overlay tests proving missing chunks enter pending state and ready chunks
  change viewport pixels.
- CLI/export tests for base-only, chunk-missing, and chunk-ready states.
