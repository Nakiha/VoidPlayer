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
  -> MetalPresentationBackend::draw_frame()
  -> renderer-owned BGRA CVPixelBuffer / IOSurface target
  -> runner-managed native video layer
```

The old Rust/native backend is removed from the active tree. `native-metal` is no
longer a supported macOS presentation mode in this worktree.

## Current Capability Boundary

Software and CPU fallback frames are rendered through the native Metal path.
VideoToolbox hardware decode can still be probed through FFmpeg-owned
hwdownload, but renderer-owned CVPixelBuffer zero-copy import is intentionally
disabled until it is rebuilt directly on native Metal.

Unsupported renderer-owned hardware paths must fail closed and remain visible in
diagnostics; they must not silently fall back to Flutter Texture video
presentation.

## Runner Boundary

The runner should allocate and own presentation targets/layers, then install the
native video target into the renderer. The renderer may write video pixels, mark
frame completion, and publish diagnostics. It must not:

- render the video into Flutter's texture registry as the product path;
- wait on or drive Flutter's present loop;
- composite Flutter UI into the video target;
- depend on Flutter dirty state to make video visible.

The native compositor and Flutter overlay should be tested as two independent
surfaces composed by the runner.

## Required Validation

For current native backend work:

```bash
python dev.py build --native
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
python dev.py mac-ui-test --build ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv
```

When native Metal CVPixelBuffer import is rebuilt, add a new targeted hardware
decode canary before restoring any 4K60/nightly VideoToolbox requirement.
