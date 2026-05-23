# macOS Presentation Adapter

The macOS presentation boundary starts at `vr::TextureFrame` and ends at the
BGRA `CVPixelBuffer` surface owned by the macOS runner. Demux, decode, seek,
loop, audio, and playback clock policy stay in shared native code.

## Current Adapter

`native/macos/presentation_adapter.*` exposes the current BGRA conversion
adapter, and `native/macos/metal_pixel_buffer_uploader.mm` owns the Metal
staging upload:

- adapter name: `cvpixelbuffer-bgra-copy`
- input: shared `vr::TextureFrame` storage
- output: caller-provided BGRA rows, currently either a native `MTLBuffer`
  staged into a `CVPixelBuffer` or a locked `CVPixelBuffer` fallback
- supported storage: CPU RGBA, CPU planar YUV420 8-bit, CPU NV12 8-bit, CPU P010 10-bit
- unsupported storage: renderer-owned GPU textures

The Swift runner only owns `CVPixelBuffer` lifecycle, texture registration,
diagnostic counters, and Flutter frame notifications. It asks native code to
copy the current frame into the native Metal uploader, or into a locked pixel
buffer when the direct-copy fallback is enabled, and receives frame timing
metadata. The runner creates Metal-compatible, IOSurface-backed pixel buffers,
but native owns the Metal device, command queue, `CVMetalTextureCache`
validation, shared `MTLBuffer`, and blit. The same surface is used for
`presentationUploadMode=metal-bgra-staging-upload`, where native copies into a
shared `MTLBuffer` and Metal blits into the texture-backed `CVPixelBuffer`.

## Parity Expectations

These expectations are CPU-side baselines for the current native Metal staging
path and the future renderer-owned shader path:

- preserve BGRA channel order
- preserve full-range vs limited-range YUV behavior
- preserve BT.601/BT.709/BT.2020 YUV matrix selection
- preserve padded destination stride semantics
- preserve odd-dimension frames through even-coded NV12 packing metadata
- preserve planar YUV420 plane strides, widths, and heights
- keep unsupported formats visible as adapter failures until implemented
- keep the software adapter and Metal-capable surface visible in diagnostics

`macos_presentation_adapter_smoke`, `software_bgra_converter_smoke`, and
`software_frame_packer_smoke` cover these baselines in portable macOS CTest,
including matrix-aware BT.709 samples for the CPU presentation fallback.
macOS UI smoke also asserts `presentationAdapter=cvpixelbuffer-bgra-copy`,
`presentationUploadMode=metal-bgra-staging-upload`, `metalTextureValid=true`,
`pixelBufferMetalUploadCount >= 1`, `hardwareDecodeProvider=VideoToolbox`,
`hardwareDecodeActive=true`, `hardwareDecodeDownloadsToCpu=true`,
`decodeMode=videotoolbox-download-to-cpu`, and `softwareFallbackActive=false`
for the H.264 fixture. Unsupported codecs or initialization failures must keep
the fallback visible by flipping the decode-mode diagnostics instead of silently
changing playback state.

## M5 Rule

Metal work should replace the presentation adapter implementation, not create a
second playback backend. VideoToolbox should wait until this adapter boundary
has deterministic parity coverage against software presentation.
