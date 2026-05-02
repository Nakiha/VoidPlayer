# VBS Legacy Format

VBS stores VTM-derived block statistics used for frame-level and CU-level
analysis. This document describes the retired VBS2 `.vbs2` layout. The native
runtime now reads [VBS3](VBS3.md) and no longer ships a VBS2 reader.

The historical C++ reader used packed little-endian structs. The names below
are kept for format archaeology only; they are not current runtime APIs.

## Producer And Reader

- Producer: instrumented VTM `DecoderApp`
- Reader: removed; use `vr::analysis::Vbs3File`
- File extension: `.vbs2`
- Legacy magic: `VBS2`

VBI + VBT can still be loaded without VBS3 for packet/NALU views, but frame
summary and bucket APIs require VBS3 and do not synthesize frame rows from VBI.

## VBS2 Layout

```text
Vbs2Header
frame data blob
  Vbs2FrameHeader
  CU record[num_cus]
  ...
Vbs2IndexEntry[num_frames] at header.index_offset
```

VBS2 has a frame-level index. Each `Vbs2IndexEntry` points to the start of a
`Vbs2FrameHeader` inside the frame data blob.

## Header: `Vbs2Header` (16 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBS2`. |
| `width` | `uint16_t` | Video width reported by VTM. |
| `height` | `uint16_t` | Video height reported by VTM. |
| `num_frames` | `uint32_t` | Number of indexed frame records. |
| `index_offset` | `uint32_t` | Byte offset of the `Vbs2IndexEntry` array. |

Parser guardrails:

- `num_frames <= 10,000,000`
- index table must fit within the file
- each index entry must point to a readable `Vbs2FrameHeader`

The 32-bit `index_offset` and 32-bit frame offsets are a known VBS2 limitation
for very large files.

## Index Entry: `Vbs2IndexEntry` (8 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `offset` | `uint32_t` | File offset of the frame's `Vbs2FrameHeader`. |
| `num_cus` | `uint32_t` | Number of CU records for the frame. |

Parser guardrails:

- `num_cus <= 2,000,000`
- `num_cus` must match the frame header's `num_cus`
- the minimum frame byte span must fit within the file

## Frame Header: `Vbs2FrameHeader` (134 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `poc` | `int32_t` | Codec picture order count, or `-1` sentinel. |
| `num_cus` | `int32_t` | Number of CU records following this header. |
| `temporal_id` | `uint8_t` | Codec temporal id. |
| `slice_type` | `uint8_t` | `0 = B`, `1 = P`, `2 = I`. |
| `nal_unit_type` | `uint8_t` | Codec NAL unit type. |
| `avg_qp` | `uint8_t` | Average frame QP. |
| `num_ref_l0` | `uint8_t` | Number of L0 references, max 15. |
| `num_ref_l1` | `uint8_t` | Number of L1 references, max 15. |
| `ref_pocs_l0` | `int32_t[15]` | L0 reference POCs. |
| `ref_pocs_l1` | `int32_t[15]` | L1 reference POCs. |

## CU Records

Each CU record starts with `Vbs2CuCommon` and may be followed by a mode-specific
extension.

### Common CU: `Vbs2CuCommon` (9 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `x` | `uint16_t` | CU x coordinate. |
| `y` | `uint16_t` | CU y coordinate. |
| `w` | `uint8_t` | CU width. |
| `h` | `uint8_t` | CU height. |
| `depth` | `uint8_t` | CU depth. |
| `qp` | `uint8_t` | CU QP. |
| `pred_mode` | `uint8_t` | `0 = inter`, `1 = intra`, `2 = IBC`, `3 = PLT`. |

### Intra Extension: `Vbs2CuIntra` (3 bytes)

Present when `pred_mode == 1`.

| Field | Type |
|---|---:|
| `intra_mode` | `uint8_t` |
| `mip_flag` | `uint8_t` |
| `isp_mode` | `uint8_t` |

### Inter Extension: `Vbs2CuInter` (13 bytes)

Present when `pred_mode == 0`.

| Field | Type |
|---|---:|
| `skip` | `uint8_t` |
| `merge_flag` | `uint8_t` |
| `inter_dir` | `uint8_t` |
| `mv_l0_x` | `int16_t` |
| `mv_l0_y` | `int16_t` |
| `mv_l1_x` | `int16_t` |
| `mv_l1_y` | `int16_t` |
| `ref_l0` | `int8_t` |
| `ref_l1` | `int8_t` |

`pred_mode == 2` and `pred_mode == 3` currently have no extension in the VBS2
reader.

## Notes

- the removed `Vbs2File::open()` reader read the frame index and validated each
  indexed frame header, but it did not load all CU records into memory.
- `read_frame_header(i)` sought directly to the indexed frame header.
- `read_frame(i)` sought to the indexed frame and read the full CU payload.
- VBS2 does not contain a compact frame-summary table or CU-level secondary
  index. This is the main reason large analysis files are expensive for UI
  range and bucket workflows.
- VBS3 is intended to address these limitations with 64-bit offsets plus
  dedicated summary/index sections instead of overloading the VBS2 frame blob.
  See [VBS3](VBS3.md).
