# VACHUNK Derived Analysis Chunk

VACHUNK is the target file format for derived analysis results generated from a
VAC2 base index. Chunks are optional, range-scoped, feature-scoped, and safe to
delete. The recommended file extension is `.vck`.

Runtime code writes overlay data as VACHUNK files directly. There is no
standalone legacy block-stat sidecar in the cache path.

## Purpose

A VACHUNK stores expensive data that should not block opening the analysis UI:

- NAL unit syntax detail trees
- exact frame summaries
- CU/MB/PU/TU records
- QP heatmap data
- motion-vector/prediction overlay data
- hit-test spatial indexes
- export-only advanced syntax summaries

The VAC2 base file remains the navigation map. A chunk must declare which base
file revision and source range it depends on.

## File Name

Recommended path:

```text
cache/<hash>/chunks/<kind>/<codec>_<feature-set>_<start>_<end>.vck
```

Examples:

```text
cache/<hash>/chunks/overlay/vvc_cu_qp_mv_00001234_00001289.vck
cache/<hash>/chunks/frame_summary_exact/hevc_qp_refs_00001000_00001047.vck
cache/<hash>/chunks/nalu_detail/00004567.vck
```

File names are for human inspection and cleanup. The management index in
`meta.json` should be used for authoritative discovery.

## Encoding Rules

- Little-endian.
- Packed fixed-size records where possible.
- Column-oriented streams for large per-unit data.
- Payload blocks may be compressed with zstd.
- Unknown sections must be skipped.
- Chunks are immutable after publish.

## High-Level Layout

```text
VachunkHeader
VachunkSectionEntry[section_count]
section payloads
```

## Header

Proposed `VachunkHeader`:

| Field | Type | Meaning |
| --- | ---: | --- |
| `magic` | `char[4]` | `VCK1`. |
| `version_major` | `uint16_t` | Initial value `1`. |
| `version_minor` | `uint16_t` | Initial value `0`. |
| `header_size` | `uint16_t` | Size of this header. |
| `section_entry_size` | `uint16_t` | Size of each section entry. |
| `section_count` | `uint32_t` | Number of section entries. |
| `kind` | `uint16_t` | Chunk kind enum. |
| `codec` | `uint16_t` | Same values as `AnalysisCodec`. |
| `feature_flags` | `uint64_t` | Feature-set bitmask. |
| `base_content_revision` | `uint64_t` | Required VAC2 base revision. |
| `generator_revision` | `uint64_t` | Analyzer implementation revision. |
| `track_index` | `uint16_t` | Source video stream index. |
| `reserved0` | `uint16_t` | Must be zero. |
| `start_frame` | `uint32_t` | First covered frame, or `UINT32_MAX`. |
| `end_frame` | `uint32_t` | Inclusive last covered frame, or `UINT32_MAX`. |
| `start_packet` | `uint32_t` | First covered packet, or `UINT32_MAX`. |
| `end_packet` | `uint32_t` | Inclusive last covered packet, or `UINT32_MAX`. |
| `start_unit` | `uint32_t` | First covered bitstream unit, or `UINT32_MAX`. |
| `end_unit` | `uint32_t` | Inclusive last unit, or `UINT32_MAX`. |
| `section_table_offset` | `uint64_t` | Absolute offset of the section table. |
| `file_size` | `uint64_t` | Expected final file size. |
| `checksum` | `uint64_t` | Optional whole-file or payload checksum. |
| `reserved1` | `uint64_t[4]` | Must be zero. |

The exact C++ struct size should be locked by `static_assert` when implemented.

## Section Entry

Proposed `VachunkSectionEntry`:

| Field | Type | Meaning |
| --- | ---: | --- |
| `type` | `char[4]` | Section FourCC. |
| `flags` | `uint32_t` | Section-specific flags. |
| `offset` | `uint64_t` | Absolute payload offset. |
| `size` | `uint64_t` | Payload byte size. |
| `entry_size` | `uint32_t` | Fixed record size, or `0`. |
| `entry_count` | `uint32_t` | Record count, or `0`. |
| `decoded_size` | `uint64_t` | Decoded size if compressed, otherwise `size`. |
| `checksum` | `uint64_t` | Optional payload checksum. |
| `reserved` | `uint64_t` | Must be zero. |

## Chunk Kinds

| Value | Kind | Meaning |
| ---: | --- | --- |
| `1` | `nalu_detail` | Syntax tree/details for one or more bitstream units. |
| `2` | `frame_summary_exact` | Exact frame-level summaries, such as CU-derived QP. |
| `3` | `overlay` | Geometry/scalar streams for overlay rendering. |
| `4` | `hit_test` | Spatial indexes for hover/click lookup. |
| `5` | `export` | Extra analysis data used mainly by CLI/agent export. |

## Common Sections

| FourCC | Name | Payload |
| --- | --- | --- |
| `META` | Metadata | UTF-8 JSON with generator details and diagnostics. |
| `RANG` | Range Map | Per-frame/per-unit coverage and status. |

Chunks may contain kind-specific sections in addition to common sections.

## NAL Detail Chunk

Kind: `nalu_detail`

Suggested sections:

| FourCC | Name | Payload |
| --- | --- | --- |
| `NROW` | Detail Rows | `VachunkNaluDetailRow[]`. |
| `STRS` | String Table | UTF-8 strings for field names/descriptions. |
| `RAWD` | Optional Raw Bytes | Small copied byte ranges for display/export. |

Rows should represent a tree:

| Field | Type | Meaning |
| --- | ---: | --- |
| `unit_index` | `uint32_t` | VAC2 `BSU2` index. |
| `parent_row` | `uint32_t` | Parent row index, or `UINT32_MAX`. |
| `depth` | `uint16_t` | Display nesting depth. |
| `flags` | `uint16_t` | Error, inferred, important, condition row, etc. |
| `field_name_string` | `uint32_t` | String table offset/id. |
| `description_string` | `uint32_t` | String table offset/id, or zero. |
| `value_kind` | `uint16_t` | Integer, signed, bool, enum, string, bytes, rational. |
| `bit_width` | `uint16_t` | Number of parsed bits when known. |
| `bit_offset` | `uint64_t` | Bit offset from unit payload start when known. |
| `value_u64` | `uint64_t` | Inline numeric value or string/bytes reference. |

NAL detail chunks are ideal for the professional field/value inspector.

## Exact Frame Summary Chunk

Kind: `frame_summary_exact`

Suggested sections:

| FourCC | Name | Payload |
| --- | --- | --- |
| `FSUM` | Exact Frame Summary | VAC2-compatible exact `FSUM` rows. |
| `BKT2` | Exact Buckets | Optional exact buckets for large ranges. |

This chunk can upgrade base QP values from `slice/base/estimated` to `exact`
without requiring overlay geometry.

## Overlay Chunk

Kind: `overlay`

Suggested sections:

| FourCC | Name | Payload |
| --- | --- | --- |
| `FIDX` | Frame Index | Maps covered frames to local records. |
| `UIDX` | Unit Index | Per-unit row ranges and optional parent links. |
| `CPAY` | Column Payload | Compressed column streams. |
| `HTST` | Optional Hit-Test Index | Spatial bins or tree data. |

The overlay payload should be column-oriented, range-scoped, and feature-scoped.

Recommended initial overlay feature bits:

| Bit | Feature |
| ---: | --- |
| `0` | CU/MB geometry. |
| `1` | QP scalar. |
| `2` | prediction mode. |
| `3` | motion vectors / prediction lines. |
| `4` | reference indexes. |
| `5` | PU geometry. |
| `6` | TU geometry. |
| `7` | codec tool flags. |
| `8` | bit/cost scalar. |

## Overlay Frame Index

Each covered frame should map to a local unit range:

| Field | Type | Meaning |
| --- | ---: | --- |
| `frame_index` | `uint32_t` | VAC2 frame index. |
| `first_unit` | `uint32_t` | First local overlay unit. |
| `unit_count` | `uint32_t` | Number of local overlay units. |
| `first_stream_value` | `uint32_t` | Optional first column-stream value. |
| `flags` | `uint32_t` | Complete, partial, inferred, exact. |
| `reserved` | `uint32_t` | Must be zero. |

## Overlay Unit Columns

Initial columns should cover current overlay data:

- unit type: MB/CU/PU/TU
- parent unit id
- x, y, width, height
- depth
- QP
- prediction mode
- intra mode
- skip/merge/inter direction
- L0/L1 reference indexes
- L0/L1 motion vectors

Future columns may add affine control points, transform flags, coded bits/cost,
VVC tool flags, or export-only syntax values.

## Generation And Publish

1. Acquire the chunk generation lock.
2. Resolve request range through VAC2.
3. Decode/parse into memory or a temp file under `tmp/`.
4. Write a complete `.vck.tmp`.
5. Validate header, ranges, feature flags, and checksums.
6. Atomically rename to `.vck`.
7. Register the chunk as complete in `meta.json`.

Readers must not open temp files. Readers should verify that the chunk
`base_content_revision` matches `base.vac`.

## Cache Management

- Chunks can be deleted independently from `base.vac`.
- LRU pruning should prefer chunks with large byte size and old access time.
- `nalu_detail` chunks are cheap and may have a longer retention policy.
- `overlay` chunks are large and should be pruned aggressively.
- `frame_summary_exact` chunks are valuable for charts/export and can be kept
  longer than overlay chunks.

## CLI And Agent Behavior

CLI/export tools should report capability state explicitly:

- `base`: only VAC2 base data was used.
- `partial`: some requested chunks were available or generated.
- `complete`: all requested derived chunks were available or generated.
- `missing`: a requested capability cannot be produced for this codec/file.

Tools should avoid silently substituting base QP for exact QP unless the output
labels the QP quality.
