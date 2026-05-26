# Color Pipeline

VoidPlayer native has one shared color contract and multiple platform
presentation backends. The shared contract is: decode metadata and frame storage
must be normalized into shader inputs that produce equivalent SDR BGRA/RGB output
on Windows D3D11 and macOS Metal.

Native does not currently provide HDR passthrough, Windows HDR metadata, ICC
profiles, display-profile transforms, or per-monitor color management. PQ/HLG
sources are tone-mapped to SDR before presentation. System compositors and the
Flutter engine own the final display mapping after the native texture is
published.

## Shared Output Contract

- Output is SDR BGRA/RGB-compatible content for a Flutter texture.
- Shader output is SDR/sRGB code value, not a platform display-profile managed
  signal.
- SDR YUV sources produce nonlinear R'G'B' values.
- HDR tone-map branches convert to linear BT.709, then explicitly encode to
  sRGB-like SDR output.
- Software decode, hardware decode, hwdownload, and fallback package paths must
  converge on equivalent shader metadata and sample layout for the same source.

Concrete presentation targets are platform-specific:

| Platform | Backend target | Notes |
| --- | --- | --- |
| Windows | D3D11 BGRA shared texture / optional swap chain | Uses DXGI formats and HLSL shaders. |
| macOS | Metal-rendered BGRA `CVPixelBuffer` / IOSurface | Exposed to Flutter through the macOS texture registrar. |

True HDR output would require Flutter surface, platform swapchain/texture,
metadata, tone mapping, and display-management changes beyond the current native
renderer contract.

## Frame Format Policy

`FrameConverter` does not use `libswscale` or `libyuv` as a generic fallback.
Supported formats are converted or packed explicitly so layout and range
behavior remain testable.

| Input format | Shared shader input | Notes |
| --- | --- | --- |
| `YUV420P`, `YUVJ420P` | CPU planar Y/U/V | Three 8-bit planes sampled directly. |
| `NV12` | CPU NV12 | Y and interleaved UV are copied directly. |
| `NV21` | CPU NV12 | VU is swapped to UV. |
| `YUV422P`, `YUVJ422P` | CPU NV12 | Chroma is vertically downsampled to 4:2:0. |
| `YUV444P`, `YUVJ444P` | CPU NV12 | Chroma is 2x2 downsampled to 4:2:0. |
| `YUV420P10LE` | CPU P010 | 10-bit planar data is packed into high-bit P010 layout. |
| `P010LE` | CPU P010 | P010 high-bit layout is preserved. |
| `YUV422P10LE` | CPU P010 | Chroma is vertically downsampled to 4:2:0. |
| `YUV444P10LE` | CPU P010 | Chroma is 2x2 downsampled to 4:2:0. |
| D3D11VA NV12/P010/P016 | GPU plane textures | Windows renderer-owned direct path. |
| VideoToolbox NV12/P010 `CVPixelBuffer` | `CVPixelBuffer` fast path | macOS renderer-owned Metal path when supported. |
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

`FrameConverter` reads these fields from `AVFrame` and carries them into
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

## Windows HLSL / D3D11 Path

Windows samples shader inputs through D3D11 SRVs:

- 8-bit planar YUV420 software frames use `R8` plane textures.
- NV12 uses Y `R8` and UV `R8G8` plane SRVs.
- P010/P016 uses Y `R16` and UV `R16G16` plane SRVs.
- BGRA fallback uses a BGRA texture.
- `shaders/multitrack.hlsl` includes `shaders/color_pipeline.hlsl` for range,
  matrix, primaries, transfer, tone mapping, and final BGRA output.

The Windows headless backend currently publishes a BGRA shared texture to
Flutter. That concrete DXGI target is a Windows backend detail, not the shared
native color contract.

## macOS Metal / CVPixelBuffer Path

macOS uses the same metadata and layout contract through Metal:

- VideoToolbox zero-copy frames keep their `CVPixelBuffer` storage when the
  codec and pixel format are supported by the renderer-owned Metal path.
- Software/fallback frames use explicit YUV or BGRA present packages.
- The Metal uploader validates target size, BGRA pixel-buffer compatibility,
  `CVMetalTextureCache` wrapping, storage kind, and package dimensions before
  draw.
- The renderer-owned target is a Metal-compatible, IOSurface-backed BGRA
  `CVPixelBuffer` registered with Flutter by Swift.
- Swift does not apply color policy; it only owns texture lifecycle and frame
  notification.

The macOS Metal shader path must stay equivalent to the Windows D3D11/HLSL path
for range expansion, matrix selection, odd-dimension chroma packing, P010
high-bit interpretation, and SDR tone mapping. Unsupported package kinds should
fail visibly through presentation diagnostics rather than silently changing
decode or playback policy.

## Shader Conversion

The shared shader contract covers:

- limited/full range expansion
- BT.601 / BT.709 / BT.2020_NCL YUV -> RGB
- BT.2020 primaries to BT.709 conversion
- PQ / HLG tone-map to SDR/sRGB code value
- BGRA output for the platform texture target

BT.601 primaries currently preserve RGB code values without a separate gamut
conversion. The ordinary SDR branch keeps a historical `1/255` slight downward
adjustment to preserve old software decode rounding parity. It is about one
8-bit code value and should not be interpreted as HDR or full/limited range
correction.

The tone mapper is a stable preview mapping, not a full film-grade HDR pipeline.
It does not read mastering display metadata or MaxCLL, does not adapt to target
display peak brightness, and does not implement player/driver enhancements.
Changing curves requires golden or capture tests that compare software and
hardware output for the same source.

## Parity Gates

Open stabilization gates before raising macOS release confidence:

- CPU reference vs Metal output for planar YUV420, NV12, P010, and BGRA package
  paths.
- D3D11 HLSL vs Metal shader parity for range, matrix, transfer, and odd
  dimensions.
- Full/limited range parity for BT.601, BT.709, and BT.2020_NCL.
- P010 high-bit interpretation parity across software, VideoToolbox, and D3D11VA
  paths.
- 4:2:2 / 4:4:4 fallback behavior remains explicit until dedicated direct paths
  are implemented.
- Capture/hash tests use backend capture contracts where available, not only
  Flutter texture screenshots.

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
