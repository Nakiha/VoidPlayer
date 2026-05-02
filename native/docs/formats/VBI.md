# VBI Format

VBI stores bitstream-unit index records used by analysis views. Files use the
`.vbi` extension. The extension is stable across format revisions; the on-disk
magic distinguishes legacy `VBI1` from current `VBI2`.

The authoritative C++ layout is defined in
`native/analysis/parsers/binary_types.h`. All fields are little-endian and the
on-disk structs are packed with `#pragma pack(push, 1)`.

## Producer And Reader

- Producer: `vr::analysis::AnalysisGenerator`
- Reader: `vr::analysis::VbiFile`
- File extension: `.vbi`
- Supported magic: `VBI1` legacy, `VBI2` current
- Current writer magic: `VBI2`

`VbiFile::open()` loads all entries into memory. `VBI1` is normalized into the
current in-memory `VbiHeader` shape with `version = 1`.

## VBI2 Layout

```text
VbiHeader
optional future header bytes when header_size > sizeof(VbiHeader)
VbiEntry[num_units]
```

The entry array starts at byte offset `header_size`.

## Header: `VbiHeader` (56 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBI2`. |
| `version` | `uint16_t` | Must be `2`. |
| `codec` | `uint16_t` | `VbiCodec`. |
| `unit_kind` | `uint16_t` | `VbiUnitKind`. |
| `header_size` | `uint16_t` | Header byte size. Current writer uses `sizeof(VbiHeader)`. |
| `num_units` | `uint32_t` | Number of `VbiEntry` records. |
| `source_size` | `uint64_t` | Source byte span covered by indexed units. |
| `reserved` | `uint8_t[32]` | Reserved, written as zero. |

Parser guardrails:

- `version == 2`
- `header_size >= sizeof(VbiHeader)`
- `num_units <= 10,000,000`
- file must contain `num_units * sizeof(VbiEntry)` bytes after `header_size`

## Legacy Header: `VbiLegacyHeader` (16 bytes)

`VBI1` files are still accepted for compatibility.

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBI1`. |
| `num_nalus` | `uint32_t` | Number of following entries. |
| `source_size` | `uint32_t` | Legacy 32-bit source size. |
| `reserved` | `uint32_t` | Reserved. |

Legacy entries use the same current `VbiEntry` layout.

## Entry: `VbiEntry` (16 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `offset` | `uint64_t` | Byte offset of the unit/start code in the indexed source span. |
| `size` | `uint32_t` | Unit byte size. |
| `nal_type` | `uint8_t` | Codec-specific unit type, kept for ABI compatibility. |
| `temporal_id` | `uint8_t` | Codec-specific temporal id when available. |
| `layer_id` | `uint8_t` | Codec-specific layer id when available. |
| `flags` | `uint8_t` | Bit flags. |

Flags:

| Bit | Constant | Meaning |
|---:|---|---|
| `0x01` | `VBI_FLAG_IS_VCL` | Unit is coded video data. |
| `0x02` | `VBI_FLAG_IS_SLICE` | Unit is a slice unit when the codec exposes that concept. |
| `0x04` | `VBI_FLAG_IS_KEYFRAME` | Unit represents or belongs to a keyframe/random access point. |

## Enumerations

`VbiCodec` values:

| Value | Codec |
|---:|---|
| `0` | Unknown |
| `1` | H.264 |
| `2` | HEVC/H.265 |
| `3` | VVC/H.266 |
| `4` | AV1 |
| `5` | VP9 |
| `6` | MPEG-2 |

`VbiUnitKind` values:

| Value | Kind |
|---:|---|
| `0` | Unknown |
| `1` | NALU |
| `2` | OBU |
| `3` | Start code |
| `4` | Packet |

## Notes

- The `.vbi` extension does not imply the legacy `VBI1` format.
- Current generated `.vbi` files are `VBI2`.
- For MP4-style H.264/HEVC/VVC samples, `AnalysisGenerator` uses FFmpeg
  Annex-B bitstream filters when available so VBI indexes stable start-code
  units rather than container length prefixes.
