# VBS3 Format

VBS3 is the current native VBS format for VTM-derived block statistics. It is
written by the instrumented VTM `DecoderApp` when `VTM_BINARY_STATS_FORMAT=VBS3`
is set, and read by `vr::analysis::Vbs3File`.

## Why VBS3 Exists

VBS2 is useful because it keeps VTM block statistics out of the slow text trace
path, but its layout is still shaped like an append-only frame blob:

- the only index is a frame-to-blob offset table at the end of the file
- frame summaries are embedded inside each frame blob, so range queries require
  many seeks
- CU data and frame-level UI data are coupled, even when the UI only needs
  compact frame rows or timeline buckets
- all file offsets are 32-bit, which is fragile for long or high-resolution
  streams
- there is no section directory for optional indexes, summaries, checksums, or
  future extensions

VBS3 separates compact summary data from heavy CU payloads. The intended open
path is: read the fixed header, read the section directory, map or read the
frame summary table, and load CU payloads only when a frame-level detail view or
overlay needs them.

## Compatibility Goals

- Keep VBI and VBT semantics unchanged. VBS3 is still optional.
- Native runtime reads `.vbs3`; `.vbs2` is legacy and is not used as a fallback.
- Keep frame order aligned with VBT packet/frame order used by the current
  analysis FFI.
- Keep the first CU payload revision close to VBS2 so the VTM-side writer can
  be migrated without rewriting the statistic extraction logic.
- Use 64-bit offsets everywhere in VBS3.

## Producer And Reader

- Producer: instrumented VTM `DecoderApp`
- Writer switch: `VTM_BINARY_STATS=<path>` plus
  `VTM_BINARY_STATS_FORMAT=VBS3`
- Reader: `vr::analysis::Vbs3File`
- File extension: `.vbs3`
- Magic: `VBS3`

## Layout

All fields are little-endian. On-disk structs must be packed with
`#pragma pack(push, 1)`.

```text
Vbs3Header
section payloads...
Vbs3SectionEntry[section_count]
```

The section table is addressed by `Vbs3Header.section_table_offset`. Section
payloads can be written in any order; readers must locate data by section type.
This lets the VTM writer stream CU payloads first, then append summaries and the
directory when frame counts and offsets are final.

## Header: `Vbs3Header` (64 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBS3`. |
| `version_major` | `uint16_t` | Major format version, initially `3`. |
| `version_minor` | `uint16_t` | Minor format version, initially `0`. |
| `header_size` | `uint16_t` | Size of this header, initially `64`. |
| `section_entry_size` | `uint16_t` | Size of each section entry, initially `48`. |
| `flags` | `uint32_t` | File-level flags; `0` for the initial writer. |
| `width` | `uint32_t` | Video width reported by VTM. |
| `height` | `uint32_t` | Video height reported by VTM. |
| `frame_count` | `uint32_t` | Number of frame summary rows. |
| `section_count` | `uint32_t` | Number of section directory entries. |
| `section_table_offset` | `uint64_t` | File offset of the section directory. |
| `file_size` | `uint64_t` | Final file size in bytes. |
| `content_revision` | `uint64_t` | Writer-controlled revision, initially `0`. |
| `reserved` | `uint64_t` | Must be zero. |

## Section Entry: `Vbs3SectionEntry` (48 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `type` | `char[4]` | Section fourcc. |
| `flags` | `uint32_t` | Section-specific flags. |
| `offset` | `uint64_t` | File offset of the section payload. |
| `size` | `uint64_t` | Section payload byte size. |
| `entry_size` | `uint32_t` | Fixed record size, or `0` for variable payloads. |
| `entry_count` | `uint32_t` | Number of records, or `0` for variable payloads. |
| `checksum` | `uint64_t` | Optional checksum; `0` means absent. |
| `reserved` | `uint64_t` | Must be zero. |

Required section types:

| Type | Name | Required | Meaning |
|---|---|---:|---|
| `FSUM` | Frame Summary | Yes | Fixed frame summary rows, one per frame. |
| `CUID` | CU Index | Yes if `CUBL` exists | Fixed CU payload index rows, one per frame. |
| `CUBL` | CU Blob | No | Variable CU records grouped by frame. |

Optional section types:

| Type | Name | Meaning |
|---|---|---|
| `BSUM` | Bucket Summary | Precomputed timeline/range aggregates. |
| `CSPX` | CU Spatial Index | Per-frame spatial/tile index for viewport overlays. |
| `META` | Metadata | UTF-8 JSON or another explicitly versioned metadata blob. |

Unknown section types must be skipped.

## Frame Summary: `Vbs3FrameSummary` (160 bytes)

`FSUM` is the fast path for analysis lists, charts, and bucket generation. It
must be contiguous and fixed-width.

| Field | Type | Meaning |
|---|---:|---|
| `poc` | `int32_t` | Codec picture order count. |
| `coded_order` | `uint32_t` | Zero-based frame row index. |
| `vcl_nalu_index` | `uint32_t` | Matching VBI VCL index, or `UINT32_MAX` if unknown. |
| `flags` | `uint32_t` | Frame flags; initially `0`. |
| `temporal_id` | `uint8_t` | Codec temporal id. |
| `slice_type` | `uint8_t` | `0 = B`, `1 = P`, `2 = I`. |
| `nal_unit_type` | `uint8_t` | Codec NAL unit type. |
| `avg_qp` | `uint8_t` | Average frame QP. |
| `num_ref_l0` | `uint8_t` | Number of L0 references, max 15. |
| `num_ref_l1` | `uint8_t` | Number of L1 references, max 15. |
| `qp_min` | `uint8_t` | Minimum CU QP in this frame, or `0` if unavailable. |
| `qp_max` | `uint8_t` | Maximum CU QP in this frame, or `0` if unavailable. |
| `ref_pocs_l0` | `int32_t[15]` | L0 reference POCs. |
| `ref_pocs_l1` | `int32_t[15]` | L1 reference POCs. |
| `num_cus` | `uint32_t` | Number of CU records for this frame. |
| `cu_index_entry` | `uint32_t` | Matching `CUID` row, normally same as `coded_order`. |
| `reserved` | `uint32_t[2]` | Must be zero. |

## CU Index: `Vbs3CuIndexEntry` (24 bytes)

`CUID` lets the reader jump to one frame's CU payload without scanning earlier
frames.

| Field | Type | Meaning |
|---|---:|---|
| `offset` | `uint64_t` | File offset inside `CUBL` section payload. |
| `byte_size` | `uint64_t` | Number of payload bytes for the frame. |
| `cu_count` | `uint32_t` | Number of CU records in the frame. |
| `flags` | `uint32_t` | Payload flags; initially `0`. |

The offset is relative to the beginning of the `CUBL` payload, not the beginning
of the file.

## CU Blob

The first VBS3 CU payload revision reuses the legacy VBS2 variable CU
records:

```text
VbsCuCommon
optional VbsCuIntra when pred_mode == 1
optional VbsCuInter when pred_mode == 0
```

This keeps the initial VTM writer change small: the extraction code that fills
CU common/intra/inter fields can remain mostly unchanged, while the writer gains
64-bit section offsets and separate frame summaries.

If the CU payload changes later, the `CUBL` section flags or a dedicated payload
revision field must make that explicit before readers interpret records.

## Bucket Summary

`BSUM` is optional. It exists to avoid recomputing timeline aggregates from
every frame on large files. The first implementation can omit it and still get
the main VBS3 benefit from contiguous `FSUM` rows. When added, bucket rows should
declare their bucket size in the section metadata or flags so multiple bucket
levels can coexist.

## Generation Notes For The VTM Subrepo

The format-aware VTM writer lives in:

`native/analysis/vendor/vtm/source/Lib/CommonLib/dtrace_blockstatistics.cpp`

Shared flow:

- `binaryStatsPath()` reads `VTM_BINARY_STATS`
- `binaryStatsFormat()` reads `VTM_BINARY_STATS_FORMAT`
- global `BinaryStatsState g_binStats` opens the selected writer
- `writeAllCodedDataBinary()` appends CU records while traversing CUs

VBS3 generation uses the same CU extraction loop with a VBS3 writer:

- choose VBS2 or VBS3 by `VTM_BINARY_STATS_FORMAT`
- use `_ftelli64` / `_fseeki64` on Windows for section offsets
- stream `CUBL` first while collecting `Vbs3FrameSummary` and
  `Vbs3CuIndexEntry` rows in memory
- append `FSUM`, `CUID`, and the section directory in `finalize()`
- patch `Vbs3Header` at file offset zero

`DecoderApp` and `DecCu` already use `VTM_BINARY_STATS` as the switch for
binary-stats-only shortcuts, so the initial VBS3 writer does not need a separate
decoder control path.
