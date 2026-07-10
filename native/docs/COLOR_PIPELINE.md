# Color Pipeline

VoidPlayer native has one shared color contract and multiple platform
presentation backends. The shared contract is: decode metadata and frame storage
must be normalized into shader inputs that produce equivalent RGB output for the
selected platform presentation target.

The historical production target is SDR BGRA/RGB for a Flutter texture. The
macOS HDR exploration path adds a native compositor target that outputs extended
linear Display P3 into a `RGBA16Float` `CAMetalLayer`. Windows native
presentation is reserved/fail-closed on this branch; a new D3D11/DX12 color
contract must be documented when that backend is rebuilt.

## Shared Output Contract

- The shared renderer carries range, matrix, transfer, and primaries as metadata
  into platform presentation decisions.
- SDR Flutter texture output remains SDR BGRA/RGB-compatible content.
- macOS native-compositor EDR output is linear Display P3, normalized so
  reference white is `1.0` and values above `1.0` represent EDR headroom.
- SDR YUV sources produce nonlinear R'G'B' values.
- HDR EDR branches decode transfer to linear light, normalize to the native
  strategy reference white, convert primaries to the output working gamut, and
  present float RGB.
- HDR SDR fallback branches convert to linear BT.709, tone-map, then explicitly
  encode to sRGB-like SDR output.
- Software decode, hardware decode, hwdownload, and fallback package paths must
  converge on equivalent shader metadata and sample layout for the same source.

Concrete presentation targets are platform-specific:

| Platform | Backend target | Notes |
| --- | --- | --- |
| Windows | reserved native D3D11/DX12 backend | Disabled on this branch; Flutter Texture SDR is not an allowed video fallback. |
| macOS SDR | Metal-rendered BGRA `CVPixelBuffer` / IOSurface | Exposed to Flutter through the macOS texture registrar. |
| macOS EDR | Native compositor `RGBA16Float` `CAMetalLayer` | Uses `extendedLinearDisplayP3` and composites native video with the exported Flutter texture. |

The HDR branch is deliberately native-compositor based on macOS. Flutter's
texture path remains an SDR integration surface unless the Flutter engine fork
later exposes a display-managed HDR surface contract.

## macOS HDR Strategy

The macOS EDR strategy is centralized in
`native/renderer/color/color_strategy.h` and mirrored by the Metal shaders:

| Stage | Current policy |
| --- | --- |
| Decode metadata | Preserve FFmpeg/VideoToolbox range, matrix, transfer, and primaries. Unknown values fall back as documented below. |
| YUV sampling | Expand limited/full range and apply BT.601, BT.709, or BT.2020 non-constant-luminance matrix. |
| SDR transfer | Treat SDR RGB as sRGB-like and decode to linear for EDR composition. |
| PQ transfer | Decode ST.2084 to absolute nits, then divide by `203.0` reference-white nits. |
| HLG transfer | Decode HLG OETF to relative linear light, then scale by `4.0` for initial EDR headroom. |
| EDR gamut | Convert BT.601, BT.709, and BT.2020 linear RGB to Display P3 linear RGB for macOS native-compositor output. |
| SDR fallback | Convert BT.2020 linear RGB to BT.709, apply Reinhard tone mapping for HDR, and encode to sRGB-like output. |
| Composition | Native video and Flutter SDR overlay are composed by the macOS native compositor after the viewport hole is applied. |

This is not yet a subjective grading layer. It is the deterministic color
correctness baseline that lets us compare CPU reference, Metal package upload,
VideoToolbox `CVPixelBuffer` upload, and later Windows shader output. Creative
or user-facing controls should be added after this baseline, for example as a
separate tone curve, exposure, or saturation stage that is tested independently.

## Frame Format Policy

`FrameConverter` does not use `libswscale` or `libyuv` as a generic fallback.
Supported formats are converted or packed explicitly so layout and range
behavior remain testable.

| Input format | Shared shader input | Notes |
| --- | --- | --- |
| `YUV420P`, `YUVJ420P` | CPU planar Y/U/V ref | Three 8-bit planes sampled directly. |
| `NV12` | CPU semiplanar Y/UV ref | Y and interleaved UV are sampled directly. |
| `NV21` | CPU NV12 | VU is swapped to UV. |
| `YUV422P`, `YUVJ422P` | CPU NV12 | Chroma is vertically downsampled to 4:2:0. |
| `YUV444P`, `YUVJ444P` | CPU NV12 | Chroma is 2x2 downsampled to 4:2:0. |
| `YUV420P10LE` | CPU planar 10-bit Y/U/V ref | Low-aligned 10-bit planes are sampled directly. |
| `P010LE` | CPU semiplanar P010 ref | P010 high-bit layout is sampled directly. |
| `YUV422P10LE` | CPU P010 | Chroma is vertically downsampled to 4:2:0. |
| `YUV444P10LE` | CPU P010 | Chroma is 2x2 downsampled to 4:2:0. |
| D3D11VA NV12/P010/P016 | GPU plane textures | Windows renderer-owned direct path. |
| VideoToolbox NV12/P010 `CVPixelBuffer` | `CVPixelBuffer` fast path | macOS renderer-owned native-metal path when supported. |
| BGRA package | BGRA texture/package | Fallback, capture, and parity path. |

4:2:2 and 4:4:4 software frames are currently displayed after downsampling to
4:2:0. Hardware surfaces such as `Y210`, `Y216`, `Y410`, `Y416`, or `AYUV` must
not be sampled as NV12/P010. Until dedicated shader paths exist, those sources
should use software decode or explicit fallback packages.

Soft/hard parity requirement:

| Source/decode output | Software path before shader | Hardware direct path before shader |
| --- | --- | --- |
| 8-bit 4:2:0 | Planar 8-bit Y/U/V or NV12 | D3D11/VideoToolbox NV12 |
| 10-bit 4:2:0 | P010 | D3D11/VideoToolbox P010/P016-compatible plane input |
| 8/10-bit 4:2:2 | Software-only downsample to NV12/P010 | Direct path disabled until native supports matching layout |
| 8/10-bit 4:4:4 | Software-only downsample to NV12/P010 | Direct path disabled until native supports matching layout |

## Color Metadata

The macOS runner probes `AVCodecParameters` before creating the renderer-owned
target so Auto presentation can decide whether a track needs EDR. `DemuxThread`
also stores the same stream-level metadata in `TrackInfo`, while
`FrameConverter` reads per-frame `AVFrame` metadata and carries it into
presentation packages and renderer-owned frames:

| Metadata | Supported values |
| --- | --- |
| Range | limited, full |
| Matrix | BT.601, BT.709, BT.2020 non-constant-luminance |
| Transfer | SDR, PQ, HLG |
| Primaries | BT.601, BT.709, BT.2020 |

Defaults:

- Unknown range defaults to limited.
- `YUVJ420P`, `YUVJ422P`, and `YUVJ444P` default to full range when metadata is
  missing.
- Unknown matrix is inferred from resolution: width `>= 1280` or height `> 576`
  uses BT.709; smaller content uses BT.601.
- Unknown transfer defaults to SDR.
- Unknown primaries are inferred from the matrix.

Dolby Vision dynamic metadata / RPU is not consumed yet. Dolby Vision profile 8
and similar files are displayed through their base HLG/PQ layer when FFmpeg
reports that transfer metadata; full Dolby Vision grading remains future work.

## Windows native D3D Path

Windows native presentation is reserved/fail-closed on this restart branch. The
old D3D11/DComp/D3D12 color path, source projection, HDR promotion,
cross-adapter handling, and overlay composition evidence were removed from the
active docs and gates.

When Windows work resumes, the new D3D11/DX12 sandwich backend must document:

- shader input resource layout for software, hwdownload, and hardware frames;
- SDR/HDR output formats, reference white, transfer handling, and tone mapping;
- how the runner composites the exported premultiplied-alpha Flutter surface
  over native video without color keys, child HWND holes, or desktop capture;
- deterministic parity tests against the shared color reference and macOS
  native-metal behavior.

## macOS native-metal / CVPixelBuffer Path

macOS uses the same metadata and layout contract through native-metal:

- VideoToolbox zero-copy frames keep their `CVPixelBuffer` storage when the
  codec and pixel format are supported by the renderer-owned native-metal path.
- VideoToolbox renderer-owned direct decode is gated to 4:2:0-like stream
  formats before codec open; 4:2:2 / 4:4:4 streams fall back to software decode
  until dedicated CVPixelBuffer/shader layouts exist.
- Software/fallback frames use explicit YUV or BGRA present packages.
- The macOS uploader validates target size, BGRA pixel-buffer compatibility,
  `CVMetalTextureCache` wrapping, storage kind, and package dimensions before
  draw.
- The SDR renderer-owned target is a Metal-compatible, IOSurface-backed BGRA
  `CVPixelBuffer` registered with Flutter by Swift.
- The EDR native-compositor target is `RGBA16Float` and is interpreted by
  `CAMetalLayer` as extended linear Display P3.
- Swift does not apply color policy; it only owns texture lifecycle and frame
  notification, except for selecting the native compositor layer color space.

Local VideoToolbox probing on Apple Silicon showed that macOS can return more
than NV12/P010-style 4:2:0 surfaces. The probe decoded synthetic H.264, HEVC,
VP9, and ProRes streams with VideoToolbox and recorded the first decoded
`CVPixelBuffer` format:

| Source format | VideoToolbox format | Plane layout |
| --- | --- | --- |
| 8-bit 4:2:0 limited / full | `420v` / `420f` | Y full resolution, CbCr half width and half height. |
| 10-bit 4:2:0 limited / full | `x420` / `xf20` | Y full resolution, CbCr half width and half height. |
| 8-bit 4:2:2 limited / full | `422v` / `422f` | Y full resolution, CbCr half width and full height. |
| 10-bit 4:2:2 limited / full | `x422` / `xf22` | Y full resolution, CbCr half width and full height. |
| 8-bit 4:4:4 limited / full | `444v` / `444f` | Y full resolution, CbCr full width and full height. |
| 10-bit 4:4:4 limited / full | `x444` / `xf44` | Y full resolution, CbCr full width and full height. |
| ProRes 4444 | `y416` | Packed 16-bit RGBA/YUVA-like surface with no CoreVideo planes. |

The current direct path only accepts the first two rows. Supporting the rest is
incremental rather than a renderer rewrite, but it requires a real format table:
`CVPixelBuffer` OSType -> chroma layout, bit depth, range expectation, plane
wrapping, shader sampling, CPU reference, and parity tests. The safest expansion
order is `422v/422f/x422/xf22` first, then `444v/444f/x444/xf44`, and packed
formats such as `y416` last. Until then, 4:2:2 / 4:4:4 streams are deliberately
kept on the software fallback path.

The macOS Metal shader path must stay equivalent to the CPU reference for range
expansion, matrix selection, odd-dimension chroma packing, P010 high-bit
interpretation, EDR transfer/gamut mapping, and SDR tone mapping. Unsupported
package kinds should fail visibly through presentation diagnostics rather than
silently changing decode or playback policy.

## Shader Conversion

The shared shader contract covers:

- limited/full range expansion
- BT.601 / BT.709 / BT.2020_NCL YUV -> RGB
- BT.2020 primaries to Display P3 conversion for macOS EDR output
- BT.2020 primaries to BT.709 conversion for SDR fallback output
- PQ / HLG mapping to EDR float output or tone-map to SDR/sRGB code value
- BGRA/RGBA output for the platform texture target

BT.601, BT.709, and BT.2020 primaries are converted into Display P3 for the
macOS EDR branch. The ordinary SDR branch keeps a historical `1/255` slight
downward adjustment to preserve old software decode rounding parity. It is
about one 8-bit code value and should not be interpreted as HDR or full/limited
range correction.

The tone mapper is a stable preview mapping, not a full film-grade HDR pipeline.
It does not read mastering display metadata or MaxCLL, does not adapt to target
display peak brightness, and does not implement player/driver enhancements.
Changing curves requires golden or capture tests that compare software and
hardware output for the same source.

## Parity Gates

Current Windows status:

- Windows native presentation is reserved/fail-closed in the back-to-native
  restart branch. The old D3D11/DComp/D3D12 preservation evidence was removed
  from active gates and must be replaced with a fresh D3D11/DX12 matrix when
  the Windows runner-composed sandwich backend is rebuilt.

Current macOS release-readiness evidence:

- macOS native-metal color/layout parity is covered by targeted macOS UI
  capture smokes and native color reference smoke. It
  compares BGRA channel order, NV12/P010 paths, split/layout fit, VideoToolbox
  CVPixelBuffer source import, and headed capture diagnostics against shared
  CPU/reference expectations.
- `native_4k60_playback_smoke.csv` remains a headed VideoToolbox/Metal cadence
  canary. It is not a strict 4K60 SLA; it asserts conservative health signals
  such as monotonic PTS, no large PTS gaps, duplicate PTS visibility, host
  interval samples/max/p95, and a high renderer-owned presentation ratio.

Required evidence before raising macOS release confidence further:

- New Windows shader vs Metal shader parity for range, matrix, transfer, and
  odd dimensions once the Windows backend exists.
- Full/limited range parity for BT.601, BT.709, and BT.2020_NCL.
- P010 high-bit interpretation parity across VideoToolbox and D3D11VA direct
  surfaces, beyond the synthetic CPU P010 package gate.
- 4:2:2 / 4:4:4 fallback behavior remains explicit until dedicated direct paths
  are implemented.
- Capture/hash expansion for real media should use backend capture contracts
  where available, not only Flutter texture screenshots.

## MHW Full-Range BT.709 Fixture

`resources/video/mhw_hevc_fullrange_bt709_3s.mp4` is a portable fixture stream
copied from a local Monster Hunter Wilds 4K sample. `ffprobe` metadata:

| Field | Value |
| --- | --- |
| Pixel format | `yuv420p` |
| Range | `pc` / full |
| Matrix | `bt709` |
| Transfer | `bt709` |
| Primaries | `bt709` |
| Resolution | `3840x2160` |

It is SDR full-range BT.709, not HDR/PQ/HLG.
`ui_tests/color/hevc_fullrange_bt709_decode_mode_single_track_diff.csv` compares
force software decode and preferred hardware decode final BGRA capture, covering
full-range metadata parity between software and hardware paths.

In the 2026-05-11 manual check, `build/color_hevc_full_soft.png` and
`build/color_hevc_full_hard.png` were identical. Compared with an FFmpeg RGB
reference for the same 1920x1080 frame, average absolute RGB difference was
about one code value and average luma was effectively unchanged. If this fixture
looks darker than PotPlayer, first compare the other player renderer, range,
color-management, and enhancement settings before assuming native soft/hard YUV
metadata handling is wrong.
