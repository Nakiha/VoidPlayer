# macOS Presentation Adapter

The macOS presentation boundary starts at `vr::TextureFrame` and ends at a
locked BGRA destination owned by the macOS runner. Demux, decode, seek, loop,
audio, and playback clock policy stay in shared native code.

## Current Adapter

`native/macos/presentation_adapter.*` exposes the current software adapter:

- adapter name: `cvpixelbuffer-bgra-copy`
- input: shared `vr::TextureFrame` storage
- output: caller-provided BGRA rows, currently a locked `CVPixelBuffer`
- supported storage: CPU RGBA, CPU planar YUV420 8-bit, CPU NV12 8-bit
- unsupported storage: P010 and renderer-owned GPU textures

The Swift runner only owns `CVPixelBuffer` lifecycle, locking, texture
registration, and Flutter frame notifications. It asks native code to copy the
current frame into the locked pixel buffer and receives frame timing metadata.
The runner now creates Metal-compatible, IOSurface-backed pixel buffers and
validates that CoreVideo can wrap them through a `CVMetalTextureCache`; that
surface validation is diagnostic-only until the adapter replaces CPU BGRA copy
with a real Metal presentation path.

## Parity Expectations

These expectations are CPU-side baselines for the future Metal/CVPixelBuffer
adapter:

- preserve BGRA channel order
- preserve full-range vs limited-range YUV behavior
- preserve padded destination stride semantics
- preserve odd-dimension frames through even-coded NV12 packing metadata
- preserve planar YUV420 plane strides, widths, and heights
- keep unsupported formats visible as adapter failures until implemented
- keep the software adapter and Metal-capable surface visible in diagnostics

`macos_presentation_adapter_smoke`, `software_bgra_converter_smoke`, and
`software_frame_packer_smoke` cover these baselines in portable macOS CTest.
macOS UI smoke also asserts `presentationAdapter=cvpixelbuffer-bgra-copy` and
`metalTextureCreationCount >= 1`, so future M5 work must explicitly move the
diagnostic contract when the Metal adapter becomes active.

## M5 Rule

Metal work should replace the presentation adapter implementation, not create a
second playback backend. VideoToolbox should wait until this adapter boundary
has deterministic parity coverage against software presentation.
