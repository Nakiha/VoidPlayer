# VAC Analysis Container

VAC is the current cache container for native analysis data. It replaces the
runtime cache set of separate `.vbs3`, `.vbi`, and `.vbt` files with one
`<hash>.vac` file.

The container is intentionally simple: it stores complete existing payloads as
sections. VBI2, VBT1, and VBS3 keep their own internal formats and can still be
tested independently, while cache management and runtime loading only need one
file.

## Producer And Reader

- Producer: `naki_analysis_generate`
- Writer helper: `vr::analysis::write_analysis_container`
- Reader: `vr::analysis::AnalysisContainerFile`
- File extension: `.vac`
- Magic: `VAC1`

## Layout

```text
AnalysisContainerHeader
AnalysisContainerSectionEntry[section_count]
section payloads
  VBS3 bytes (optional, required for VVC frame summaries)
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
| `VBS3` | Optional | Complete VBS3 block statistics bytes. Required for VVC frame/charts. |

## Generation Notes

The current generator still has two stages:

1. VVC only: extract a temporary Annex-B `.tmp.vvc` and let VTM write temporary
   `.tmp.vbs3`.
2. FFmpeg pass: write temporary `.tmp.vbi` and `.tmp.vbt`.
3. Pack the temporary files into `<hash>.vac`, then delete the temporary
   analysis files.

The temporary VVC input exists only for the VTM `DecoderApp` process. Removing
that staging file requires changing how VTM receives input.
