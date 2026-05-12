# VAC1 Analysis Container (Legacy)

VAC1 is the retired single-file cache container for native analysis data. It
replaced the older runtime cache set of separate `.vbs4`, `.vbi`, and `.vbt`
files with one `<hash>.vac` file.

Runtime cache generation now uses [VAC2](VAC2.md) base files plus
[VACHUNK](VACHUNK.md) derived chunks. This document is kept for parser tests and
for understanding old artifacts that may be deleted during cache cleanup.

The container is intentionally simple: it stores complete existing payloads as
sections. VBI2, VBT1, and VBS4 keep their own internal formats and can still be
tested independently, while cache management and runtime loading only need one file.

## Producer And Reader

- Producer: none in current runtime
- Writer helper: `vr::analysis::write_analysis_container`
- Reader: `vr::analysis::AnalysisContainerFile`
- File extension: `.vac`
- Magic: `VAC1`

## Layout

```text
AnalysisContainerHeader
AnalysisContainerSectionEntry[section_count]
section payloads
  VBS4 bytes (optional section, block statistics)
  VBI2 bytes
  VBT1 bytes
```

All offsets in the section table are absolute file offsets from the beginning
of the `.vac` file. Section payload bytes are unmodified inner files.

## Header: `AnalysisContainerHeader` (64 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `magic` | `char[4]` | Must be `VAC1`. |
| `version` | `uint16_t` | Current version, `1`. |
| `header_size` | `uint16_t` | `sizeof(AnalysisContainerHeader)`. |
| `section_entry_size` | `uint16_t` | `sizeof(AnalysisContainerSectionEntry)`. |
| `section_count` | `uint16_t` | Number of section entries. |
| `flags` | `uint32_t` | Reserved, currently zero. |
| `section_table_offset` | `uint64_t` | Absolute offset of the section table. |
| `file_size` | `uint64_t` | Expected full container size. |
| `reserved` | `uint64_t[4]` | Reserved, zero-filled. |

Readers must treat any `version` other than the compiled
`kAnalysisContainerVersion` as incompatible. Flutter cache validation deletes
that hash's stale analysis artifacts before regeneration; native readers reject
the container instead of trying to partially load it.

## Section Entry: `AnalysisContainerSectionEntry` (48 bytes)

| Field | Type | Meaning |
|---|---:|---|
| `type` | `char[4]` | Section FourCC. |
| `flags` | `uint32_t` | Reserved per-section flags. |
| `offset` | `uint64_t` | Absolute payload offset. |
| `size` | `uint64_t` | Payload byte size. |
| `checksum` | `uint64_t` | Reserved, currently zero. |
| `reserved` | `uint64_t[2]` | Reserved, zero-filled. |

## Section Types

| FourCC | Required | Payload |
|---|---:|---|
| `VBI2` | Yes | Complete VBI2 bitstream-unit index bytes. |
| `VBT1` | Yes | Complete VBT1 packet timing bytes. |
| `VBS4` | Optional | Complete [VBS4](VBS4.md) block statistics bytes from the codec-specific analyzer. Required for VVC/HEVC/H.264 frame charts. |

## Historical Generation Notes

The old generator had these stages:

1. Codec-specific VBS4 producer: VVC extracts or streams Annex-B input to VTM;
   HEVC/H.264 are handled by the instrumented FFmpeg analyzer.
2. FFmpeg pass: write temporary `.tmp.vbi` and `.tmp.vbt`.
3. Pack the temporary files into `<hash>.vac`, then delete the temporary
   analysis files.

The temporary VVC input exists only for the VTM `DecoderApp` process. Removing
that staging file requires changing how VTM receives input.
