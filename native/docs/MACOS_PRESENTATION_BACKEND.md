# macOS Presentation Backend

This branch treats macOS as the first implementation of the runner-composed
native sandwich:

```text
NSWindow / runner view
  -> native Metal video layer or IOSurface-backed target
  -> Flutter engine premultiplied-alpha ARGB surface
  -> runner/CoreAnimation composition
```

Flutter owns Flutter rendering and present scheduling. Native owns video decode,
layout, color, and the video texture. The runner owns final composition.

See [SANDWICH_RENDERING.md](SANDWICH_RENDERING.md) for the cross-platform
contract.

## Active Native Path

The active renderer backend is `RenderBackendKind::Metal`.

```text
RendererDrawSnapshot
  -> native SourceCompositorLease
  -> MetalPresentationBackend::draw_frame()
  -> native-owned BGRA8/RGBA16F CVPixelBuffer / IOSurface rings
  -> complete retained source package
  -> runner-managed Metal video layer
```

The old Rust/native backend is removed from the active tree. `native-metal` is no
longer a supported macOS presentation mode in this worktree.

## Current Capability Boundary

Software frames and VideoToolbox decode output are normalized by the native
Metal source-bake path into compositor-ready SDR BGRA8 or linear EDR RGBA16F
IOSurfaces. The final runner compositor does not decode color or reinterpret
source transfer functions.

Unsupported hardware formats must fail closed and remain visible in
diagnostics; they must not silently fall back to Flutter Texture video
presentation.

## Runner Boundary

Native owns source presentation resources and package publication. The runner
owns the destination layer, retains the latest complete source package, applies
projection, and performs final composition. Neither side may:

- render the video into Flutter's texture registry as the product path;
- wait on or drive Flutter's present loop;
- composite Flutter UI into the video target;
- depend on Flutter dirty state to make video visible;
- rebuild source resource rings from Swift-side policy.

The native compositor and Flutter overlay should be tested as two independent
surfaces composed by the runner.

## Metal Pipeline Startup

The runner compiles `VoidPlayerNativeShaders.metal` into the app's
`default.metallib`. Native pipeline creation loads that precompiled library and
is prewarmed on a background queue while Flutter starts, so media creation does
not invoke Metal's runtime source compiler.

The generated MSL include remains only for standalone native tests built with
`BUILD_TESTS=ON`. Product runner builds compile with runtime shader fallback
disabled; a missing or stale app metallib therefore fails closed instead of
silently restoring the old first-frame compile stall.

## Required Validation

For current native backend work:

```bash
python dev.py build --native
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
python dev.py mac-ui-test --build ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv
```

When native Metal CVPixelBuffer import is rebuilt, add a new targeted hardware
decode canary before restoring any 4K60/nightly VideoToolbox requirement.
