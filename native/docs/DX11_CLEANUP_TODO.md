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

- [ ] Stop exposing raw D3D11 synchronization details through `NativePlayer`.
  - Remove public `texture_mutex()` forwarding from `NativePlayer`.
  - Prefer snapshot/capture APIs such as `acquire_shared_texture()` that carry
    texture and shared handle together.

- [ ] Move public D3D11 types behind a renderer backend API.
  - `RendererConfig` currently contains `IDXGIAdapter*`.
  - `SharedTextureSnapshot` currently contains `ID3D11Texture2D*` and `HANDLE`.
  - These are acceptable for the Windows backend, but should not be part of a
    platform-neutral renderer interface.

- [ ] Split target ownership in CMake.
  - Keep D3D11/DXGI/D3DCompiler/WinMM links in a Windows backend/plugin target.
  - Keep media, playback, analysis, and shared utility code available without
    the D3D11 renderer target.

## P1: D3D11 Runtime Contract

- [ ] Document and enforce the minimum D3D11 feature level.
  - The current device creation path allows down to 9_1, while shaders,
    shared textures, and D3D11VA have a higher practical requirement.
  - Decide and enforce the real minimum before treating D3D11 as a stable
    backend contract.

- [ ] Add a device-lost state machine.
  - Current code records `device_lost` and removed reason.
  - Required next state model: `Lost -> TearDownPipelines -> RecreateDevice ->
    RecreateTextures -> ReprimeDecode`.
  - Until recovery exists, surface device lost as a terminal renderer error.

- [ ] Modernize or retire the windowed swap-chain path.
  - It currently uses `DXGI_SWAP_EFFECT_DISCARD`.
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
