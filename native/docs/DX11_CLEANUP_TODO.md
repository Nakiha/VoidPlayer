# DX11 Cleanup TODO

This list tracks native cleanup work pulled from `build/GPT_native.md` that
directly affects the D3D11/DXGI backend and the future cross-platform native
base. The goal is not to delete the Windows renderer, but to make it a clean
backend behind explicit platform boundaries before adding a macOS backend.

## P0: Backend Boundary

- [x] Make headless texture sharing require the Flutter DXGI adapter.
  - Headless mode is only valid when native and Flutter can share DXGI handles
    on the same adapter.
  - Missing adapter or adapter-specific device creation must fail explicitly,
    not fall back to an unrelated D3D11 device that may later black-screen.

- [x] Stop exposing raw D3D11 synchronization details through `NativePlayer`.
  - Remove public `texture_mutex()` forwarding from `NativePlayer`.
  - Prefer snapshot/capture APIs such as `acquire_shared_texture()` that carry
    texture and shared handle together.

- [x] Move public D3D11 types behind a renderer backend API.
  - `RendererConfig` now carries backend interop as opaque handles instead of
    `IDXGIAdapter*`.
  - `SharedTextureSnapshot` now carries typed native handles instead of
    `ID3D11Texture2D*` and `HANDLE`.
  - `renderer.h` no longer includes D3D11 backend headers or exposes COM render
    resources; Windows implementation details live in the cpp/backend layer.

- [x] Split target ownership in CMake.
  - Keep D3D11/DXGI/D3DCompiler/WinMM links in a Windows backend/plugin target.
  - Keep media, playback, analysis, and shared utility code available without
    the D3D11 renderer target.
  - `video_renderer_core` now owns media/playback/buffer/sync/shared utility
    sources without D3D11/DXGI/WinMM links; `video_renderer_lib` owns the
    Windows renderer, Windows audio output factory, D3D11 backend, D3D11VA
    decode provider, and WinMM/D3D links.

## P1: D3D11 Runtime Contract

- [x] Document and enforce the minimum D3D11 feature level.
  - The current device creation path allows down to 9_1, while shaders,
    shared textures, and D3D11VA have a higher practical requirement.
  - Decide and enforce the real minimum before treating D3D11 as a stable
    backend contract.

- [x] Add a device-lost state machine.
  - `D3D11Device` now polls and records device removal from present/resize and
    removed-reason checks.
  - `Renderer` promotes device loss to `Ready -> Lost -> Terminal`, stops
    playback/rendering, and exposes the terminal state until shutdown.
  - Future automatic recovery can expand this into `TearDownPipelines ->
    RecreateDevice -> RecreateTextures -> ReprimeDecode`.

- [x] Modernize or retire the windowed swap-chain path.
  - Windowed mode now uses `DXGI_SWAP_EFFECT_FLIP_DISCARD`.
  - Either update to a flip-model swap chain or clearly mark windowed mode as a
    dev/demo path separate from Flutter headless rendering.

## P2: Performance and Diagnostics

- [ ] Pool D3D11 snapshot/capture textures.
  - Hardware snapshot exact seek currently creates temporary textures in a hot
    path.

- [ ] Add D3D11 backend metrics.
  - Track render wait, copy time, present/publish time, shared texture resize
    count, device-lost count, and texture-sharing failures.

- [ ] Strengthen D3D11 tests around failure paths.
  - Missing Flutter adapter in headless mode.
  - Adapter-specific device creation failure.
  - Texture sharing failure.
  - Device removed/lost reporting.
