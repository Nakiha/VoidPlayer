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

## Source Compositor Contract

The shared renderer contract owns only platform-neutral state:

- track identity and dimensions;
- projection and retained visual clipping;
- ring budget policy;
- output pixel/color/alpha semantics;
- topology, ring, and frame generations;
- package completeness and lifecycle transitions.

Platform resource leases remain outside the shared contract. A macOS lease may
carry `CVPixelBuffer`, `IOSurface`, and Metal objects. A Windows lease may carry
D3D resources, shared handles, and synchronization primitives. Both leases
must carry metadata compatible with `SourceCompositorPackageMetadata`, but the
shared renderer must not include either platform resource type.

The source texture is compositor-ready. SDR BGRA8 samples must not be decoded
again by the runner compositor. EDR RGBA16F samples are linear extended-range
values. Native video sources are opaque; Flutter remains the only
premultiplied-alpha layer in the sandwich.

The shared lifecycle is generation driven:

```text
Unconfigured -> Allocating -> Ready -> Publishing -> Draining -> Unconfigured
```

Reconfiguration begins with a newer topology generation. Publication requires
the current topology and ring generations plus a strictly increasing frame
generation. Incomplete packages never replace the last complete published
package.

## macOS Target

The first implementation target is:

```text
NSWindow / content view
  -> native Metal video layer or IOSurface-backed target
  -> transparent FlutterView / Flutter engine ARGB surface
  -> CoreAnimation/runner composition
```

The current native validation path uses renderer-owned Metal targets for
software/package frames and VideoToolbox CVPixelBuffer frames. Startup installs
the native target ring and then lets the native renderer own video cadence;
Flutter remains responsible only for its ARGB UI surface.

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
