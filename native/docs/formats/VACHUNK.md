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

Current runtime path:

```text
cache/<hash>/chunks/<kind>/<kind-id>_f<feature-mask>_b<base-revision>_g<generator-revision>_<start-frame>_<end-frame>.vck
```

Examples:

```text
cache/<hash>/chunks/overlay/3_f000000000000011f_b0000000000000001_g0000000000000002_00000128_00000191.vck
```

File names are used by Dart cache discovery to cheaply reject stale generator
revisions and locate covered frame windows. Native readers still validate the
VACHUNK header before consuming payloads.

## Encoding Rules

- Little-endian.
- Packed fixed-size records where possible.
- Column-oriented streams for large per-unit data.
- Payload sections may be compressed with zstd.
- Unknown sections must be skipped.
- Chunks are immutable after publish.

## High-Level Layout

```text
VachunkHeader
VachunkSectionEntry[section_count]
section payloads
```

## Header

Current `VachunkHeader`:

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
| `compression` | `uint16_t` | `0` none, `1` zstd. Section flags identify the compressed payloads. |
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

The C++ struct size is locked at 128 bytes by `static_assert`.

## Section Entry

Current `VachunkSectionEntry`:

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

Known generic section flags:

| Flag | Meaning |
| ---: | --- |
| `0x00000001` | Payload bytes are zstd-compressed. `size` is compressed bytes and `decoded_size` is the byte count after decompression. |

When no zstd flag is set, readers require `decoded_size == size`. The runtime
writer only compresses sections when zstd wins by a small margin, so tiny or
poorly-compressing sections remain raw.

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
| `FSUM` | Frame Summary | `VachunkFrameSummary[]` for exact per-frame stats. |
| `CU4R` | Overlay CU/MB Records | Variable-size `VachunkCuCommon + VachunkCuInter/Intra` records. |
| `UIDX` | Unit Index | Future per-unit row ranges and optional parent links. |
| `CPAY` | Column Payload | Future compressed column streams. |
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
- coded bit count
- prediction mode
- intra mode
- skip/merge/inter direction
- L0/L1 reference indexes
- L0/L1 motion vectors

Current runtime records store the initial overlay data as packed row records
rather than separate compressed columns:

```text
VachunkCuCommon:
  uint16 x, uint16 y, uint8 w, uint8 h
  uint8 depth, uint8 qp, uint8 pred_mode
  uint32 bit_count

VachunkCuInter:
  skip, merge_flag, inter_dir, mv_l0/l1, ref_l0/l1

VachunkCuIntra:
  intra_mode, mip_flag, isp_mode
```

`bit_count` is the syntax bit delta attributed by the codec hook to the CU/MB.
The renderer normalizes it to a 64x64 block before applying the fixed bitrate
heatmap scale.

Future columns may add affine control points, transform flags, VVC tool flags,
or export-only syntax values.

## Generation And Publish

1. Acquire the chunk generation lock.
2. Resolve request range through VAC2.
3. Decode/parse into memory or a temp file under `tmp/`.
4. Write a complete `.vck.tmp`.
5. Validate header, ranges, feature flags, and checksums.
6. Atomically publish to the final `.vck` name.
7. Touch cache metadata for LRU/cache-size management.

Readers must not open temp files. Readers should verify that the chunk
`base_content_revision` and `generator_revision` match the runtime expectation.

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
