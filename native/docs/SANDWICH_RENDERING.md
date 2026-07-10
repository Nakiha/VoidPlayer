# Runner-Composed Native Sandwich

This branch restarts presentation around one rule:

```text
native window
  -> native video texture/layer (SDR or HDR)
  -> Flutter engine ARGB UI surface
  -> runner-owned composition
```

Flutter owns Flutter rendering. Native owns video rendering. The runner owns
composition. The native renderer must not render video into a Flutter texture,
drive Flutter present scheduling, or depend on Flutter to publish video frames.

## Layer Contract

| Layer | Owner | Responsibility |
| --- | --- | --- |
| Native window | runner | Window lifetime, platform view/layer tree, input routing. |
| Native video layer | native renderer + runner | Decode output, layout/color, SDR/HDR target, video present diagnostics. |
| Flutter UI surface | Flutter engine | Widgets, viewport alpha, controls, animation, UI present scheduling. |
| Final compose | runner | Z-order, alpha blending, swap/present, platform diagnostics. |

The Flutter fork should only make the Flutter surface exportable as a real
premultiplied-alpha ARGB layer. It should not own the native video texture or
the final presentation strategy.

## Native Target Contract

The shared renderer selects frames, applies the complete multi-track layout,
and submits one `RendererDrawSnapshot` to the platform presentation backend.
The platform backend publishes one complete opaque viewport target:

- SDR uses compositor-ready BGRA8;
- HDR uses linear extended-range RGBA16F;
- pan, zoom, split, track order, background, and analysis overlay are already
  resolved in that target;
- target publication is generation based and independent of Flutter state.

There is no per-track source lease or runner-side layout projection. Flutter is
the only premultiplied-alpha layer in the final composition.

## macOS Target

The first implementation target is:

```text
NSWindow / content view
  -> native Metal video layer or IOSurface-backed target
  -> transparent FlutterView / Flutter engine ARGB surface
  -> CoreAnimation/runner composition
```

The macOS presentation target owns its `CVPixelBuffer`/`IOSurface` ring. The
runner retains the latest complete native target and samples it below Flutter's
ARGB UI surface on every display tick. Swift does not interpret video layout,
overlay primitives, or track topology.

## Windows Target

Windows is intentionally fail-closed on this branch until the same model is
rebuilt:

```text
top-level HWND
  -> native D3D11/D3D12 video visual
  -> Flutter ARGB visual exported by the locked engine
  -> DComp/runner composition
```

The reserved backend names are `native-d3d11` and `native-d3d12`. They are
interface placeholders, not active render backends.

## Deleted Paths

The removed experimental renderer backend, its UI smokes, and the old Windows
native D3D12 backend source are no longer active tree material. Use the previous
branch or main history as reference material when rebuilding missing behavior.
