# VBS4 Format Draft

VBS4 is the next-generation block-statistics payload format for codec
decoder-derived analysis data. It is intended to replace VBS3 for H.266/VVC,
H.265/HEVC, and H.264/AVC once the writer and reader are migrated.

The core change from VBS3 is that heavy block data is no longer stored as
per-frame row records. VBS4 keeps frame summaries as a fast uncompressed table,
then stores codec-specific block data in compressed frame-range blocks. Each
block uses a compact codec profile and column-oriented streams so repeated
fields, bitsets, QP deltas, motion vectors, and reference indexes compress well.

## Goals

- Keep frame list, reference pyramid, and frame trend startup fast.
- Load and decompress detailed block data only for the frame range currently
  needed by the UI.
- Compress H.264 macroblock data much better than VBS3 row records.
- Give H.265/H.266 variable block trees a native encoding instead of forcing
  every CU into one fixed common/inter/intra record shape.
- Keep VAC/VBI/VBT semantics unchanged. VAC embeds VBS4 as a separate payload
  section.
- Use no mandatory non-system runtime dependency for the first implementation.

## Why VBS3 Is Not Enough

VBS3 improved open-time behavior by splitting `FSUM` from the heavy `CUBL`
payload. Its heavy payload is still frame-oriented row data:

- per-frame compression keeps random access simple but limits compressor context
- fixed row records store many derivable fields, especially for H.264 raster
  macroblocks
- large fixed frame windows do not help much when the row layout itself is the
  dominant cost
- per-frame records are awkward for column encoders, block caches, and range
  prefetch

Recent H.264 measurements showed that simply grouping many frames before XPRESS
compression did not materially reduce file size. Compact H.264 rows helped
because they removed deterministic fields. VBS4 makes that idea first-class and
extends it to H.265/H.266.

## Container

- VAC section type: `VBS4`
- Standalone extension: `.vbs4`
- Magic: `VBS4`
- Byte order: little-endian
- On-disk structs are packed with `#pragma pack(push, 1)`

VBS3 and VBS4 can coexist during migration. Readers should prefer `VBS4` when a
VAC contains both `VBS4` and `VBS3`, and fall back to `VBS3` while producers are
being ported.

## High-Level Layout

```text
Vbs4Header
section payloads...
Vbs4SectionEntry[section_count]
```

The section table is addressed by `Vbs4Header.section_table_offset`. Section
payloads may be written in any order.

Required sections:

| Type | Name | Meaning |
|---|---|---|
| `FSUM` | Frame Summary | Fixed frame summary rows, one per decoded frame. |
| `BIDX` | Block Index | One row per compressed payload block. |
| `CPAY` | Codec Payload | Concatenated compressed block payloads. |

Recommended sections:

| Type | Name | Meaning |
|---|---|---|
| `FIDX` | Frame Index | Maps each frame to a `BIDX` row and per-frame row range inside the decoded block. |
| `BSUM` | Bucket Summary | Optional precomputed timeline aggregates for very large files. |
| `META` | Metadata | UTF-8 JSON with writer, codec profile, source, and feature flags. |

Unknown sections must be skipped.

## Header: `Vbs4Header` (80 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBS4`. |
| `version_major` | `uint16_t` | Initially `4`. |
| `version_minor` | `uint16_t` | Initially `0`. |
| `header_size` | `uint16_t` | Size of this header, initially `80`. |
| `section_entry_size` | `uint16_t` | Size of each section row, initially `56`. |
| `codec` | `uint16_t` | Same values as `VbiCodec`: H264=1, HEVC=2, VVC=3. |
| `profile` | `uint16_t` | Codec payload profile. |
| `flags` | `uint32_t` | File-level flags. |
| `width` | `uint32_t` | Coded/display width used by payload reconstruction. |
| `height` | `uint32_t` | Coded/display height used by payload reconstruction. |
| `frame_count` | `uint32_t` | Number of `FSUM` rows. |
| `block_count` | `uint32_t` | Number of `BIDX` rows. |
| `section_count` | `uint32_t` | Number of section directory rows. |
| `reserved0` | `uint32_t` | Must be zero. |
| `section_table_offset` | `uint64_t` | File offset of the section directory. |
| `file_size` | `uint64_t` | Final file size in bytes. |
| `content_revision` | `uint64_t` | Writer-controlled revision. |
| `reserved1` | `uint64_t` | Must be zero. |

## Section Entry: `Vbs4SectionEntry` (56 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `type` | `char[4]` | Section fourcc. |
| `flags` | `uint32_t` | Section-specific flags. |
| `offset` | `uint64_t` | File offset of section payload. |
| `size` | `uint64_t` | Section payload byte size. |
| `entry_size` | `uint32_t` | Fixed row size, or `0` for variable payloads. |
| `entry_count` | `uint32_t` | Row count, or `0` for variable payloads. |
| `checksum` | `uint64_t` | Optional checksum; `0` means absent. |
| `reserved0` | `uint64_t` | Must be zero. |
| `reserved1` | `uint64_t` | Must be zero. |

## Frame Summary: `FSUM`

`FSUM` stays intentionally close to VBS3. It is the fast path for:

- frame list
- reference pyramid
- frame trend
- bucket generation
- frame-to-NALU UI selection

The first version may reuse `Vbs3FrameSummary` unchanged. That keeps current
analysis FFI and chart code migration small. Future versions may add a wider
summary row only if the UI needs additional fast-path fields.

`FSUM` must not be compressed.

## Frame Index: `FIDX` (24 bytes per frame)

`FIDX` lets the reader locate one frame inside a decoded block without scanning
all previous frames.

| Field | Type | Meaning |
|---|---:|---|
| `block_index` | `uint32_t` | Matching `BIDX` row. |
| `local_frame` | `uint32_t` | Zero-based frame index inside the block. |
| `first_record` | `uint32_t` | First block/CU/MB record for this frame inside the decoded block. |
| `record_count` | `uint32_t` | Number of records for this frame. |
| `flags` | `uint32_t` | Per-frame payload flags. |
| `reserved` | `uint32_t` | Must be zero. |

`FIDX` is recommended even though `BIDX` has frame ranges. Without it, H.265 and
H.266 variable-tree frames would require a prefix sum table inside every decoded
block before one frame can be materialized.

## Block Index: `BIDX` (64 bytes per block)

`BIDX` is the random-access index for heavy payloads.

| Field | Type | Meaning |
|---|---:|---|
| `first_frame` | `uint32_t` | First frame covered by this block. |
| `frame_count` | `uint32_t` | Number of frames covered by this block. |
| `first_record` | `uint32_t` | First global record index, if useful; otherwise `0`. |
| `record_count` | `uint32_t` | Number of block/CU/MB records in the decoded block. |
| `payload_offset` | `uint64_t` | Offset inside `CPAY`. |
| `payload_size` | `uint64_t` | Compressed block byte size. |
| `decoded_size` | `uint64_t` | Decoded block byte size. |
| `codec_profile` | `uint16_t` | Payload profile for this block. |
| `compression` | `uint16_t` | `0=none`, `1=XPRESS_HUFF`, future `2=zstd`. |
| `flags` | `uint32_t` | Block flags. |
| `checksum` | `uint64_t` | Optional decoded-block checksum. |
| `reserved` | `uint64_t` | Must be zero. |

### Block Sizing

Blocks are chosen by raw decoded payload size first, not by a fixed frame count.

Initial targets:

- target decoded block size: 8-16 MiB
- hard decoded block cap: 64 MiB
- hard frame cap per block: 4096 frames
- never split one frame across blocks

This matches the Flutter resident-window model without forcing the reader to
decompress hundreds of MiB for one clicked frame. A 4096-frame UI window can
cache many small blocks through an LRU cache.

## Codec Payload: `CPAY`

`CPAY` is a concatenation of compressed VBS4 blocks. Each decoded block starts
with a small block-local header, followed by codec-profile column streams.

```text
Vbs4DecodedBlockHeader
stream directory
stream payloads
```

### Decoded Block Header

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | `BLK4`. |
| `header_size` | `uint16_t` | Size of this header. |
| `stream_entry_size` | `uint16_t` | Size of stream directory row. |
| `codec_profile` | `uint16_t` | Payload profile. |
| `stream_count` | `uint16_t` | Number of column streams. |
| `frame_count` | `uint32_t` | Frames in this block. |
| `record_count` | `uint32_t` | Records in this block. |
| `flags` | `uint32_t` | Block-local flags. |
| `reserved` | `uint64_t` | Must be zero. |

### Stream Directory

Each stream has:

| Field | Type | Meaning |
|---|---:|---|
| `stream_id` | `uint16_t` | Field/column id. |
| `encoding` | `uint16_t` | Encoding id. |
| `offset` | `uint32_t` | Offset from decoded block start. |
| `size` | `uint32_t` | Encoded stream byte size. |
| `value_count` | `uint32_t` | Number of decoded values. |
| `flags` | `uint32_t` | Stream flags. |

Stream payloads inside one block are not separately compressed in the initial
implementation. The whole decoded block is compressed once according to `BIDX`.
This keeps I/O simple and gives the compressor context across all columns in a
range. If profiling shows field-local compression wins, a future minor version
can allow per-stream compression.

## Common Stream Encodings

| Encoding | Name | Use |
|---:|---|---|
| `0` | raw bytes | Already compact streams or fixed-size arrays. |
| `1` | bitset | Booleans such as intra/skip/merge. |
| `2` | uleb128 | Unsigned sparse integers. |
| `3` | sleb128-zigzag | Signed deltas such as MVs. |
| `4` | rle-u8 | QP and small mode runs. |
| `5` | rle-s16 | Motion-vector component runs. |
| `6` | palette-u8 | Small repeated enums/reference indexes. |
| `7` | frame-prefix-u32 | Per-frame prefix counts or row starts. |

The reader expands a decoded block into lightweight column views first. It only
materializes `VbsCuRecord`-like rows when an API asks for a specific frame's
full block records.

## Codec Profiles

### `H264MB1`

H.264 has a regular raster macroblock grid. The payload does not store `x`,
`y`, `w`, `h`, or `depth`. The reader derives them from:

```text
mb_width = ceil(width / 16)
x = (record_index % mb_width) * 16
y = (record_index / mb_width) * 16
w/h = clipped to frame edges
depth = 0
```

Recommended streams:

| Stream | Meaning | Encoding |
|---|---|---|
| `frame_prefix` | First MB row for each frame | frame-prefix-u32 |
| `is_intra` | Intra/inter flag | bitset |
| `skip` | Skip flag | bitset |
| `merge` | Merge/direct style flag if available | bitset |
| `inter_dir` | L0/L1/bi direction | palette-u8 or packed 2-bit |
| `qp` | MB QP | rle-u8 |
| `intra_mode` | Intra16x16 or coarse intra mode | palette-u8 |
| `ref_l0` / `ref_l1` | Reference indexes | palette-u8 |
| `mv_l0_x/y`, `mv_l1_x/y` | Motion vector components | sleb128-zigzag or rle-s16 |

This profile is expected to beat VBS3 compact rows because every field can use
its own representation instead of paying 12 bytes per macroblock before
compression.

### `HEVCCU1`

H.265 has variable CTU/CU trees. VBS4 stores structure explicitly but still
avoids row-record padding.

Recommended streams:

| Stream | Meaning | Encoding |
|---|---|---|
| `frame_prefix` | First CU for each frame | frame-prefix-u32 |
| `x_delta` / `y_delta` | Raster or tree-order coordinate deltas | uleb128 |
| `log2_w` / `log2_h` | CU size exponents | palette-u8 |
| `depth` | Tree depth | palette-u8 |
| `pred_mode` | inter/intra/skip/etc. | palette-u8 |
| `qp_delta` | Delta from frame or previous CU QP | sleb128-zigzag or rle-u8 |
| `intra_mode` | Intra mode when present | palette-u8 |
| `skip` / `merge` | Flags | bitset |
| `inter_dir` | L0/L1/bi | palette-u8 |
| `ref_l0` / `ref_l1` | Reference indexes | palette-u8 |
| `mv_*` | Motion vectors | sleb128-zigzag or rle-s16 |

Coordinates should be delta-coded in the natural decoder traversal order. The
writer should also mark a block flag if rows are already raster-sorted so the
reader can build overlays without sorting.

### `VVCCU1`

H.266/VVC is similar to H.265 but with more CU modes and tool flags. `VVCCU1`
extends `HEVCCU1` with optional streams:

| Stream | Meaning |
|---|---|
| `tree_type` | single/dual-tree/chroma tree metadata if needed. |
| `isp_mode` | ISP mode for intra. |
| `mip_flag` | MIP flag. |
| `ibc_flag` | Intra block copy. |
| `plt_flag` | Palette mode. |
| `affine_flag` | Affine/inter tool indicator if exposed by producer. |
| `sbt_flag` | SBT indicator if useful for UI later. |

Optional streams are omitted when all values are zero/default. The stream
directory is the schema, so old readers can skip unknown streams and still show
basic CU overlays.

## Compression

Initial compression should use Windows Compression API `XPRESS_HUFF` through
dynamic `Cabinet.dll` loading. It keeps the build clean and avoids MSYS2 runtime
dependencies.

Compression is block-level:

1. Write decoded block with column streams.
2. Compress the decoded block.
3. Store compressed bytes in `CPAY`.
4. Store block metadata in `BIDX`.

If compressed size is not smaller, store the block uncompressed with
`compression=0`.

Future profiles may add zstd as an optional bundled/static dependency, but VBS4
must not require it for baseline Windows runtime support.

## Reader Strategy

Open path:

1. Read `Vbs4Header`.
2. Read section directory.
3. Read `FSUM`, `FIDX`, and `BIDX`.
4. Do not read `CPAY` until detail block records are requested.

Frame detail path:

1. Use `FIDX[frame]` to get `block_index`, `first_record`, and `record_count`.
2. Check an LRU cache for the decoded block.
3. If missing, read `BIDX[block_index]` from `CPAY` and decompress it.
4. Parse stream directory into column views.
5. Materialize only the requested frame's records.

Recommended reader cache:

- 3-6 decoded blocks by default
- cap by decoded bytes, e.g. 128 MiB
- prefetch adjacent block when chart panning is active

## Writer Strategy

Writers keep frame summaries in memory and build VBS4 payload blocks as decoded
frames complete.

Shared flow:

1. Create `FSUM` row per decoded frame.
2. Feed codec-native CU/MB facts into a profile-specific block builder.
3. Flush a block when it reaches target decoded bytes, hard decoded bytes, hard
   frame count, or end of stream.
4. Compress the block and append to `CPAY`.
5. Append `BIDX` row and `FIDX` rows.
6. At finalize, write `FSUM`, `FIDX`, `BIDX`, optional `META/BSUM`, section
   table, then patch the header.

## Migration Plan

1. Add `VBS4.md`, packed structs, and a `Vbs4File` reader.
2. Teach VAC to accept an optional `VBS4` section and prefer it over `VBS3`.
3. Implement H.264 `H264MB1` in the FFmpeg analyzer first because it has the
   largest measured payload pressure.
4. Add reader tests that compare VBS3 and VBS4 frame summaries and selected
   materialized block records.
5. Add H.265 `HEVCCU1`, then VTM/H.266 `VVCCU1`.
6. Keep VBS3 generation available until VBS4 UI and cache migration are stable.

## Open Questions

- Whether `FSUM` should remain exactly `Vbs3FrameSummary` forever or gain a
  VBS4-specific row with codec id and layer mode hints.
- Whether `BSUM` should be mandatory for very long streams.
- Whether the first VBS4 release should include only `XPRESS_HUFF` or also a
  static zstd option for better archival compression.
- Whether overlay APIs should expose column views directly instead of
  materializing row records for every requested frame.
