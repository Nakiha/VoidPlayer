# macOS Presentation Adapter

The macOS presentation boundary starts at `vr::TextureFrame` and ends at the
BGRA `CVPixelBuffer` surface owned by the macOS runner. Demux, decode, seek,
loop, audio, and playback clock policy stay in shared native code.

## Current Adapter

`native/macos/presentation_adapter.*` exposes the current software fallback
BGRA conversion adapter, and `native/macos/metal_pixel_buffer_uploader.mm` owns
the Metal layout upload:

- adapter name: `cvpixelbuffer-bgra-copy`
- input: shared `vr::TextureFrame` storage
- output: caller-provided BGRA rows for fallback copies, or CPU NV12/P010 planes
  staged into a native `MTLBuffer` and converted by the Metal layout shader
- supported storage: CPU RGBA, CPU planar YUV420 8-bit, CPU NV12 8-bit, CPU P010 10-bit
- unsupported storage: renderer-owned GPU textures
- explicit failure contract: invalid BGRA destination, unsupported storage,
  invalid/undersized storage, and owned-frame allocation failure return distinct
  native status values before the runner receives a generic copy failure

The Swift runner only owns `CVPixelBuffer` lifecycle, texture registration,
diagnostic counters, and Flutter frame notifications. It asks native code to
present the current renderer snapshot through the installed Metal target and
receives frame timing metadata. The runner creates Metal-compatible, IOSurface-backed pixel buffers,
but native owns the Metal device, command queue, `CVMetalTextureCache`
validation and shared `MTLBuffer` upload. The same surface is used for explicit
seek/step refresh and playback callbacks. `presentationUploadMode` reports the
actual storage consumed by the native uploader:
`metal-cvpixelbuffer-present-package` for retained VideoToolbox frames,
`metal-yuv-present-package` for staged NV12/P010/planar frames, and
`metal-bgra-present-package` for BGRA fallback packages. If a frame storage kind
is not yet supported by the shader path, the uploader falls back to the software
BGRA adapter and runs the same layout compute pass over a BGRA atlas.
Native validation rejects pixel buffers whose dimensions or pixel format do not
match the expected BGRA texture surface before any frame upload is attempted.
The Metal layout uploader exposes checked validation statuses for unavailable
Metal state, invalid arguments, size mismatches, unsupported non-BGRA pixel
buffers, and CVPixelBuffer-to-Metal texture wrapping failures; Swift mirrors the
last validation message in diagnostics.

## Parity Expectations

These expectations are CPU-side baselines for the current native Metal layout
path and the future renderer-owned shader path:

- preserve BGRA channel order
- preserve full-range vs limited-range YUV behavior
- preserve BT.601/BT.709/BT.2020 YUV matrix selection
- preserve padded destination stride semantics
- preserve odd-dimension frames through even-coded NV12 packing metadata
- preserve planar YUV420 plane strides, widths, and heights
- preserve frame timing metadata on every successful copy
- keep unsupported formats visible as adapter failures until implemented
- keep the software adapter and Metal-capable surface visible in diagnostics

`macos_presentation_adapter_smoke`, `macos_metal_uploader_smoke`,
`layout_geometry_smoke`, `software_bgra_converter_smoke`, and
`software_frame_packer_smoke` cover these
baselines in portable macOS CTest, including matrix-aware BT.709/BT.2020 samples,
unknown-HD-to-BT.709 fallback, odd-dimension/even-coded NV12 metadata,
planar limited/full range, unsupported GPU texture rejection, invalid P010
rejection, destination mismatch rejection, native Metal `CVPixelBuffer`
validation, shared layout shader constants, pan offset normalization, and
resize offset preservation. `macos_metal_uploader_smoke` also opens the bundled
H.264 sample and verifies that changing native zoom layout changes the Metal
layout-uploaded `CVPixelBuffer` hash.
macOS UI smoke also asserts `presentationAdapter=cvpixelbuffer-bgra-copy`,
`presentationAdapterKind=renderer-owned-metal`,
`rendererOwnedPresentationActive=true`, `metalTextureValid=true`,
seek refreshes and playback advance `pixelBufferMetalUploadCount`, the 4K H.264
canary advances `pixelBufferMetalCVPixelBufferUploadCount`,
`hardwareDecodeProvider=VideoToolbox`,
`hardwareDecodeActive=true`, `hardwareDecodeDownloadsToCpu=false`,
`decodeMode=shared-renderer-videotoolbox`, and `softwareFallbackActive=false`
for the H.264 fixture. Unsupported codecs or initialization failures must keep
the fallback visible by flipping the decode-mode diagnostics instead of silently
changing playback state. Metal surface validation failures must be visible
through `metalTextureLastError` rather than collapsing into a generic upload
failure.

## M5 Rule

Metal work should add a renderer-owned presentation adapter behind this
boundary, not create a second playback backend. The current
`cvpixelbuffer-bgra-copy` adapter remains the software fallback and parity
oracle while Metal/CVPixelBuffer output is brought up.
