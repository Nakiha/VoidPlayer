# VBT Format

VBT stores packet timing and keyframe metadata for analysis views. Files use
the `.vbt` extension and currently carry the `VBT1` magic.

The authoritative C++ layout is defined in
`native/analysis/parsers/binary_types.h`. All fields are little-endian and the
on-disk structs are packed with `#pragma pack(push, 1)`.

## Producer And Reader

- Producer: `vr::analysis::AnalysisGenerator`
- Reader: `vr::analysis::VbtFile`
- File extension: `.vbt`
- Current magic: `VBT1`

`VbtFile::open()` reads all entries into memory and builds a PTS-sorted index
for `packet_at_pts()`. The entry order in the file is packet scan order.

## Layout

```text
VbtHeader
VbtEntry[num_packets]
```

There is no trailing index section. The entry array starts immediately after
the 32-byte header.

## Header: `VbtHeader` (32 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VBT1`. |
| `num_packets` | `uint32_t` | Number of `VbtEntry` records following the header. |
| `time_base_num` | `int32_t` | FFmpeg stream time-base numerator. |
| `time_base_den` | `int32_t` | FFmpeg stream time-base denominator. |
| `reserved` | `uint8_t[16]` | Reserved, written as zero. |

Parser guardrails:

- `num_packets <= 10,000,000`
- file must contain `num_packets * sizeof(VbtEntry)` bytes after the header

## Entry: `VbtEntry` (32 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `pts` | `int64_t` | Packet PTS in the stream time base, or `0` if unavailable. |
| `dts` | `int64_t` | Packet DTS in the stream time base, or `0` if unavailable. |
| `poc` | `int32_t` | Sequential packet/frame ordinal assigned during scan. |
| `size` | `uint32_t` | Packet byte size. |
| `duration` | `uint32_t` | Packet duration in the stream time base. |
| `flags` | `uint8_t` | Bit flags. |
| `reserved` | `uint8_t[3]` | Reserved, written as zero. |

Flags:

| Bit | Constant | Meaning |
|---:|---|---|
| `0x01` | `VBT_FLAG_KEYFRAME` | Packet is marked keyframe by FFmpeg. |

## Notes

- PTS is not guaranteed to be monotonic in file order. Use `VbtFile::packet_at_pts()` for time lookup.
- `poc` is currently a scan-order ordinal, not a codec POC parsed from the bitstream.
- VBT is required for analysis loading. VBS data is optional.
