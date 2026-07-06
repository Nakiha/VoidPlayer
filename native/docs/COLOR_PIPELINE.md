# Color Pipeline

VoidPlayer native has one shared color contract and multiple platform
presentation backends. The shared contract is: decode metadata and frame storage
must be normalized into shader inputs that produce equivalent RGB output for the
selected platform presentation target.

The historical production target is SDR BGRA/RGB for a Flutter texture. The
macOS HDR exploration path adds a native compositor target that outputs extended
linear Display P3 into a `RGBA16Float` `CAMetalLayer`. Windows Auto presents
SDR media through a BGRA8 DComp target. PQ/HLG sessions on an HDR output keep
`native-compositor-scrgb` as the desired mode, but currently fall back to native
SDR because Windows does not yet keep Flutter's SDR UI as a separate
system-managed surface while presenting HDR video.

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
| Windows | wgpu/D3D12 render target with D3D12 Flutter surface import, D3D12 video/source imports, and locked-engine DComp/DXGI present bridge | Auto selects BGRA8 SDR; HDR media records a desired scRGB mode but falls back to native SDR until SDR Flutter UI composition is system-managed. Forced scRGB remains diagnostic; Flutter Texture SDR is not an allowed fallback. |
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
| VideoToolbox NV12/P010 `CVPixelBuffer` | `CVPixelBuffer` fast path | macOS renderer-owned wgpu-metal path when supported. |
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

## Windows wgpu/D3D12 Path

Windows is migrating shader input sampling to wgpu/D3D12 resources:

- 8-bit planar YUV420 software frames use `R8` plane textures.
- NV12 uses Y `R8` and UV `R8G8` plane views.
- P010/P016 uses Y `R16` and UV `R16G16` plane views.
- BGRA fallback uses a BGRA texture.
- The wgpu shader path must share the same range, matrix, primaries, transfer,
  tone mapping, and final output rules as the legacy HLSL canaries.

The compatibility pass tone-maps to the BGRA shared texture used by Flutter or
the native SDR compositor. In native-compositor modes the same
prepared source snapshot is first rendered to
`R16G16B16A16_FLOAT`:

- linear BT.709 primaries
- `1.0 = 80 nits`
- SDR/UI colors use sRGB decode and `SDRWhiteLevel / 80`
- PQ uses absolute nits divided by 80
- HLG uses the shared headroom/reference-white policy on the 80-nit scale
- FP16 values are not clamped to the SDR range

The BGRA compatibility pass rerenders from source rather than tone-mapping the
mixed FP16 texture. This keeps existing SDR layout/color canaries stable.

In forced scRGB native-compositor mode, the renderer publishes the same linear
BT.709 scRGB video contract through D3D12 resources. The wgpu final composition shader
samples the locked engine's full-window premultiplied BGRA Flutter surface,
restores straight sRGB for transfer decoding, re-premultiplies in linear light,
applies `SDRWhiteLevel / 80`, and composites it source-over the video.
Transparent viewport pixels reveal video without color keys or a rectangular
native hole. This path is diagnostic rather than the Windows Auto HDR product
route because it bakes SDR Flutter UI into an HDR target. Auto SDR instead
samples the source-rerendered BGRA compatibility texture into a BGRA8/G22
target where the compatibility bridge still requires it. Auto HDR currently
stays on that native SDR route and reports
`hdr-ui-composition-unsupported`. Windows does not submit HDR10 metadata,
custom ICC curves, or LUT corrections; Advanced Color and calibration remain
system-managed.

For source projection, each active track is rendered with identity layout into
its source-sized `R16G16B16A16_FLOAT` texture from the same
`PreparedDrawResources` snapshot. The source pass does not bake analysis
overlays. The wgpu/D3D12 composite path imports the source textures, applies
the Dart/macOS projection contract, fills missing/out-of-range UVs with the
linearized viewport background, composites video-space overlay primitives, then
composites the exported Flutter surface. Overlay colors are sRGB-decoded and
scaled by the same SDR white contract on scRGB targets, while SDR targets keep
the BGRA compatibility contract. The remaining DComp bridge presents the final
target and does not own source/overlay/Flutter composition. This preserves the order
`source video -> analysis overlay -> Flutter UI`.

## macOS wgpu-metal / CVPixelBuffer Path

macOS uses the same metadata and layout contract through wgpu-metal:

- VideoToolbox zero-copy frames keep their `CVPixelBuffer` storage when the
  codec and pixel format are supported by the renderer-owned wgpu-metal path.
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

Current Windows preservation evidence:

- `windows_d3d11_color_layout_parity_smoke` drives synthetic
  `RendererDrawSnapshot` inputs through the real D3D11 presentation backend,
  captures the renderer-owned BGRA output, and compares it with a CPU
  reference. It covers BGRA channel order, NV12 and planar YUV420
  full/limited range, P010 high-bit samples, odd dimensions, padded strides,
  aspect-fit background bars, and split/order layout.
- `windows_d3d11_fp16_scrgb_smoke` reads back RGBA16F and BGRA outputs from the
  same draw. It covers SDR white scaling, PQ/HLG, P010, BT.2020 conversion,
  values above `1.0`, odd/padded storage, background/split/order, overlay hook
  participation, and source-rerender SDR compatibility.
- `[windows_source_cache]` and `[windows_source_projection]` tests cover bundle
  leases/generations, the 384 MiB policy, split/pan/zoom, missing sources, and
  background fallback. Rebuilt source-projection UI smoke proves the wgpu/D3D12
  product path imports source/video/Flutter surfaces and preserves the same
  visible ordering.
- `[windows_high_refresh]`, `[windows_overlay_layer]`, and
  `native_high_refresh_overlay_pan_zoom.csv` validate overlay reuse,
  projection pacing, and high-refresh hot-path behavior without reviving the
  removed D3D11 retained overlay graph.

Current macOS release-readiness evidence:

- macOS wgpu-metal color/layout parity is covered by the targeted
  `macos-wgpu-metal-smoke` UI profile and native color reference smoke. It
  compares BGRA channel order, NV12/P010 paths, split/layout fit, VideoToolbox
  CVPixelBuffer source import, and headed capture diagnostics against shared
  CPU/reference expectations.
- `native_4k60_playback_smoke.csv` remains a headed VideoToolbox/Metal cadence
  canary. It is not a strict 4K60 SLA; it asserts conservative health signals
  such as monotonic PTS, no large PTS gaps, duplicate PTS visibility, host
  interval samples/max/p95, and a high renderer-owned presentation ratio.

Required evidence before raising macOS release confidence further:

- D3D11 HLSL vs Metal shader parity for range, matrix, transfer, and odd
  dimensions.
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
