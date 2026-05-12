# VAC2 Base Analysis Container

VAC2 is the base-index container for progressive analysis. It replaces
the VAC1 "complete analysis package" contract with an immutable map of the
source bitstream. Deep analysis results are stored separately as VACHUNK files;
see [VACHUNK.md](VACHUNK.md).

Current runtime cache generation writes `cache/<hash>/base.vac` VAC2 base
files. Expensive/deep results are generated as VACHUNK files.

## Purpose

VAC2 must answer four questions quickly:

1. What packets, bitstream units, access units, and frames exist?
2. Where are they in the source file or normalized bytestream?
3. Which lightweight display statistics can the UI show immediately?
4. Where should an on-demand analyzer seek to produce deeper chunks?

## File Name

Recommended path:

```text
cache/<hash>/base.vac
```

The extension remains `.vac` because this is the base analysis container for a
hash directory. Readers must use the magic/version, not the extension, to choose
the parser.

## Encoding Rules

- Little-endian.
- Packed fixed-size records for core tables.
- Variable data is stored in explicitly typed sections.
- Offsets inside the section table are absolute offsets from the beginning of
  `base.vac`.
- Unknown sections must be skipped.
- Readers must reject incompatible major versions.

## High-Level Layout

```text
Vac2Header
Vac2SectionEntry[section_count]
section payloads
```

The section table is near the front so readers can locate payloads without
scanning the whole file.

## Header

Proposed `Vac2Header`:

| Field | Type | Meaning |
| --- | ---: | --- |
| `magic` | `char[4]` | `VAC2`. |
| `version_major` | `uint16_t` | Initial value `2`. |
| `version_minor` | `uint16_t` | Initial value `0`. |
| `header_size` | `uint16_t` | Size of this header. |
| `section_entry_size` | `uint16_t` | Size of each section entry. |
| `section_count` | `uint32_t` | Number of section entries. |
| `flags` | `uint32_t` | File-level flags. |
| `codec` | `uint16_t` | Same values as `VbiCodec`. |
| `track_index` | `uint16_t` | Source video stream index. |
| `time_base_num` | `int32_t` | Stream time-base numerator. |
| `time_base_den` | `int32_t` | Stream time-base denominator. |
| `packet_count` | `uint32_t` | Number of packet records. |
| `unit_count` | `uint32_t` | Number of bitstream-unit records. |
| `au_count` | `uint32_t` | Number of access-unit/frame records. |
| `width` | `uint32_t` | Display or coded width when known. |
| `height` | `uint32_t` | Display or coded height when known. |
| `section_table_offset` | `uint64_t` | Absolute offset of the section table. |
| `file_size` | `uint64_t` | Expected final file size. |
| `source_size` | `uint64_t` | Source file size at generation time. |
| `source_mtime_unix_ms` | `int64_t` | Source mtime at generation time. |
| `content_revision` | `uint64_t` | Generator/schema revision for invalidation. |
| `reserved` | `uint64_t[4]` | Must be zero. |

The exact C++ struct size should be locked by `static_assert` when implemented.

## Section Entry

Proposed `Vac2SectionEntry`:

| Field | Type | Meaning |
| --- | ---: | --- |
| `type` | `char[4]` | Section FourCC. |
| `flags` | `uint32_t` | Section-specific flags. |
| `offset` | `uint64_t` | Absolute payload offset. |
| `size` | `uint64_t` | Payload byte size. |
| `entry_size` | `uint32_t` | Fixed record size, or `0`. |
| `entry_count` | `uint32_t` | Record count, or `0`. |
| `checksum` | `uint64_t` | Optional payload checksum; `0` means absent. |
| `reserved` | `uint64_t[2]` | Must be zero. |

## Required Sections

| FourCC | Name | Payload |
| --- | --- | --- |
| `META` | Metadata | UTF-8 JSON metadata. |
| `PKT2` | Packet Table | `Vac2PacketEntry[]`. |
| `BSU2` | Bitstream Unit Table | `Vac2BitstreamUnitEntry[]`. |
| `AUF2` | Access Unit / Frame Table | `Vac2FrameEntry[]`. |
| `FSUM` | Lightweight Frame Summary | `Vac2FrameSummaryEntry[]`. |

## Recommended Sections

| FourCC | Name | Payload |
| --- | --- | --- |
| `PSET` | Parameter-Set Snapshots | Directory plus encoded parameter-set bytes. |
| `RAP2` | Random Access Map | Frame indexes and packet/source ranges for seek starts. |
| `BKT2` | Timeline Buckets | Precomputed packet/QP/NAL histograms. |
| `STRS` | String Table | Shared UTF-8 strings. |
| `EXTR` | Source Extradata | Codec extradata copied from the demuxer. |

## Packet Table: `PKT2`

One row per demuxed packet in scan order.

| Field | Type | Meaning |
| --- | ---: | --- |
| `pts` | `int64_t` | Packet PTS in stream time base, or sentinel if absent. |
| `dts` | `int64_t` | Packet DTS in stream time base, or sentinel if absent. |
| `duration` | `uint32_t` | Duration in stream time base. |
| `size` | `uint32_t` | Packet payload bytes. |
| `stream_index` | `uint16_t` | Source stream index. |
| `flags` | `uint16_t` | Keyframe, corrupt, discard, etc. |
| `file_offset` | `uint64_t` | Source file offset if known, otherwise `UINT64_MAX`. |
| `format_offset` | `uint64_t` | Demuxer/container offset if distinct, otherwise `UINT64_MAX`. |
| `first_unit` | `uint32_t` | First `BSU2` row in this packet. |
| `unit_count` | `uint32_t` | Number of `BSU2` rows in this packet. |
| `au_index` | `uint32_t` | Owning access unit/frame index, or `UINT32_MAX`. |
| `reserved` | `uint32_t` | Must be zero. |

`file_offset` is best-effort. Some demuxers cannot provide stable byte offsets.
When unavailable, derived analyzers may need demuxer seek by timestamp plus
packet replay instead of direct byte reads.

## Bitstream Unit Table: `BSU2`

One row per NALU/OBU/start-code unit or packet-unit, depending on codec.

| Field | Type | Meaning |
| --- | ---: | --- |
| `packet_index` | `uint32_t` | Owning packet. |
| `au_index` | `uint32_t` | Owning access unit/frame. |
| `offset` | `uint64_t` | Offset in normalized bytestream or source span. |
| `size` | `uint32_t` | Unit byte size. |
| `payload_offset` | `uint32_t` | Header/prefix bytes before RBSP/OBU payload. |
| `nal_type` | `uint8_t` | Codec-specific unit type. |
| `temporal_id` | `uint8_t` | Temporal id when available. |
| `layer_id` | `uint8_t` | Layer/spatial id when available. |
| `unit_kind` | `uint8_t` | NALU/OBU/start-code/packet. |
| `flags` | `uint16_t` | VCL, slice, keyframe/RAP, parameter-set, SEI, encrypted. |
| `pset_snapshot` | `uint16_t` | Active parameter-set snapshot id, or `UINT16_MAX`. |
| `detail_hint` | `uint32_t` | Optional detail-cache hint, otherwise zero. |

The table intentionally stores only cheap header facts. Full syntax trees belong
in NAL detail chunks.

## Access Unit / Frame Table: `AUF2`

One row per decoded/displayable frame or access unit.

| Field | Type | Meaning |
| --- | ---: | --- |
| `first_packet` | `uint32_t` | First packet in the access unit. |
| `packet_count` | `uint32_t` | Packet count. |
| `first_unit` | `uint32_t` | First bitstream-unit row. |
| `unit_count` | `uint32_t` | Bitstream-unit count. |
| `pts` | `int64_t` | Representative PTS. |
| `dts` | `int64_t` | Representative DTS. |
| `duration` | `uint32_t` | Duration in stream time base. |
| `coded_order` | `uint32_t` | Decode/scan-order ordinal. |
| `display_order` | `int32_t` | Display-order ordinal if known. |
| `poc` | `int32_t` | Codec picture order count if known. |
| `frame_size` | `uint32_t` | Sum of packet/unit bytes for this AU. |
| `rap_distance` | `uint32_t` | Frames from nearest previous RAP, or `UINT32_MAX`. |
| `flags` | `uint32_t` | Keyframe/RAP, corrupt, incomplete, inferred. |

## Lightweight Frame Summary: `FSUM`

`FSUM` keeps the analysis window useful before deep chunks exist. It is a
lightweight semantic summary, not a CU/MB statistics table.

| Field | Type | Meaning |
| --- | ---: | --- |
| `poc` | `int32_t` | Codec POC or display order fallback. |
| `coded_order` | `uint32_t` | Decode/scan-order ordinal. |
| `first_vcl_unit` | `uint32_t` | First VCL `BSU2` row. |
| `flags` | `uint32_t` | Keyframe/RAP, inferred refs, exact refs, exact QP. |
| `temporal_id` | `uint8_t` | Temporal id. |
| `slice_type` | `uint8_t` | 0=B, 1=P, 2=I, 255=unknown. |
| `nal_type` | `uint8_t` | Representative VCL NAL type. |
| `qp_kind` | `uint8_t` | 0=unknown, 1=slice, 2=base, 3=estimated, 4=exact. |
| `qp_avg` | `uint8_t` | QP value when known. |
| `qp_min` | `uint8_t` | QP min when known. |
| `qp_max` | `uint8_t` | QP max when known. |
| `num_ref_l0` | `uint8_t` | Active/derived L0 ref count. |
| `num_ref_l1` | `uint8_t` | Active/derived L1 ref count. |
| `reserved0` | `uint8_t[3]` | Must be zero. |
| `ref_pocs_l0` | `int32_t[15]` | L0 reference POCs; unused slots `INT32_MIN`. |
| `ref_pocs_l1` | `int32_t[15]` | L1 reference POCs; unused slots `INT32_MIN`. |
| `summary_chunk_id` | `uint64_t` | Exact summary chunk id, or zero. |
| `reserved1` | `uint64_t[2]` | Must be zero. |

Reference pyramid can use `FSUM` immediately. QP trend can use `FSUM` when
`qp_kind != unknown`; the UI should show quality/precision when needed.

## Parameter-Set Snapshots: `PSET`

`PSET` stores active parameter-set states at packet/AU boundaries. It enables
on-demand NAL parsing without rescanning the full file.

Recommended payload:

```text
Vac2ParameterSetSnapshotEntry[snapshot_count]
parameter set byte blob
```

Each snapshot entry should include:

- snapshot id
- first packet/AU where it becomes active
- codec ids for VPS/SPS/PPS/APS/etc.
- offsets and sizes into the blob
- feature flags describing what was parsed

## Random Access Map: `RAP2`

`RAP2` lets derived analyzers find seek starts quickly.

Each entry should include:

- RAP frame index
- RAP packet index
- source offset when available
- first frame covered
- next RAP frame
- parameter-set snapshot id
- codec-specific flags such as IDR/CRA/BLA/GDR

## Timeline Buckets: `BKT2`

Buckets are optional but recommended for large files. They allow the UI and CLI
to summarize long timelines without reading millions of rows.

Suggested fields:

- first frame
- frame count
- packet size min/max/sum
- QP min/max/sum and QP quality flags
- keyframe/RAP count
- NALU type histogram offset/count

## Reader Behavior

- Load the header and section directory first.
- Validate every required section before reporting the cache usable.
- Treat missing recommended sections as degraded capability, not corruption.
- Unknown sections are skipped.
- Base analysis is valid even if no derived chunks exist.
- If `source_size` or `source_mtime_unix_ms` no longer matches the source file,
  higher layers should invalidate the hash directory.

## Relationship To VAC1

VAC1 embeds VBI2, VBT1, and optional VBS4 complete payloads. VAC2 replaces that
with richer base tables and external derived chunks.

VAC1-to-VAC2 migration was completed destructively for runtime cache writes:
new analysis generation writes VAC2 base plus chunks, and stale VAC1 files are
only treated as cleanup artifacts. VAC1 parser coverage remains for old-format
tests.
