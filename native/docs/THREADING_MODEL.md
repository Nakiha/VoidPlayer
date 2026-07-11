# Threading Model

VoidPlayer native now uses one shared playback/render scheduler with platform
presentation backends. The threading contract is owned by shared native code;
active platform behavior begins once a `RendererDrawSnapshot` is handed to a
`PresentationBackend`. macOS Metal is the active backend on this branch; Windows
D3D11/DX12 presentation is reserved/fail-closed until the runner-composed
sandwich backend is rebuilt.

## Thread Roles

Each video track owns a demux/decode pair. The render thread is shared by all
tracks and owns present cadence, seek/step refresh, EOF settling, layout
application, and backend draw calls.

```text
UI / FFI / platform host
        |
        v
NativePlayer / Renderer command surface
        |
        +-------------------+-------------------+
        |                   |                   |
        v                   v                   v
  Track 0 pipeline    Track 1 pipeline    Track N pipeline
  DemuxThread         DemuxThread         DemuxThread
  DecodeThread        DecodeThread        DecodeThread
        \                   |                   /
         \                  v                  /
          +---------- TrackBuffer ------------+
                         |
                         v
                  RenderThread
        RenderSink -> PresentDecision -> RendererDrawSnapshot
                         |
          +--------------+--------------+
          |                             |
          v                             v
 Windows PresentationBackend      macOS PresentationBackend
 reserved D3D11/DX12              Metal/CVPixelBuffer/IOSurface
```

### Demux Thread

- Reads `AVPacket` values from the media file.
- Filters to the selected stream and converts packet timestamps to microseconds.
- Writes to the bounded `PacketQueue`.
- Observes `SeekController` state and flushes packet flow on seek.
- Does not access renderer layout, presentation backend state, or GPU resources.

### Decode Thread

- Consumes `PacketQueue` and writes `TextureFrame` values to `TrackBuffer`.
- Uses the selected hardware provider or software decoder.
- Windows D3D11VA may serialize immediate-context access through the backend
  device mutex depending on decode device mode.
- macOS VideoToolbox preserves `CVPixelBuffer` frames for the native-target
  native Metal path when the codec/pixel format is supported, and otherwise falls back
  to explicit software or hwdownload packages.
- Does not call renderer public lifecycle APIs and does not access platform
  texture publication locks directly.

### Render Thread

- Owns playback cadence and deadline sleep.
- Reads the shared `Clock`, asks `RenderSink` for a `PresentDecision`, and builds
  `RendererDrawSnapshot` values.
- Calls `PresentationBackend::draw_frame()` for normal playback, seek/step,
  capture preparation, and EOF settling. Paused and interaction redraw commands
  enter the same presentation controller from the serialized host refresh
  queue.
- During playback, frame selection remains PTS-driven. Pending layout is
  consumed by either the next playback present or an independent interaction
  redraw. While interaction redraw is active, the loop continues advancing the
  latest decision but suppresses its duplicate playback present.
- Publishes success/failure state and frame callbacks after releasing renderer
  locks.
- Does not call public lifecycle APIs and never joins itself.

### Platform Host / UI Thread

- Windows runner currently owns only Win32/platform-channel glue and fail-closed
  native-player lifecycle. Native D3D11/DX12 presentation threading is reserved
  for the later runner-composed sandwich backend.
- macOS runner owns Cocoa, sandbox file access, platform channels, Flutter
  texture registration, `CVPixelBuffer` lifecycle, and frame notification.
- macOS viewport pan/zoom submits only the latest layout intent. Its display
  link drives a non-blocking complete-target redraw independently from media
  PTS cadence, with at most two interaction targets in flight. Native completion
  releases the next interaction tick; transient ring pressure retries only the
  latest revision.
- macOS seek/step/startup/paused/EOF refresh calls install or validate the
  native target, release the Swift texture lock, then request native
  refresh completion. Swift does not own playback clock, seek, loop, layout, or
  decode policy.

### Audio Path

Audio decode and the miniaudio callback follow `AudioCoordinator` state for
pause, seek, speed, and EOF. Audio code does not access D3D11, Metal, renderer
layout state, or presentation backend internals.

## Render Loop

```text
while (running) {
    current_pts = clock.current_pts_us();
    decision = render_sink.evaluate(current_pts, tracks, layout);

    if (decision.should_present) {
        snapshot = build_renderer_draw_snapshot(decision);
        result = presentation_backend.draw_frame(snapshot);
        publish_frame_or_failure(result);
    }

    sleep_until_next_deadline(decision, speed);
}
```

Deadline-based sleep prevents cumulative drift during long playback. Backend
draw time, dropped/late frames, duplicate PTS, host interval samples, upload
storage kind, and last failure reason are diagnostics, not alternate scheduling
policies.

## Lock Strategy

| Resource | Protection | Hold time |
| --- | --- | --- |
| Renderer lifecycle | `lifecycle_mutex_` | Public lifecycle entry, render thread start/stop, handle destruction |
| Renderer state | `state_mutex_` | Track list, layout, current decision, EOF/seek/loop state, presentation state snapshot |
| PacketQueue | mutex + condvar | Packet push/pop/abort |
| TrackBuffer | mutex | Frame push/peek/advance/state update |
| Clock | mutex/atomics | Time query/update |
| SeekController | mutex + atomics | Seek request/take |
| Platform GPU device | backend device mutex | Backend draw/copy/flush/fence work and platform-specific decode sharing |
| Platform texture publication | backend texture/target lock | Shared texture index, CVPixelBuffer target generation, capture/front-buffer state |
| Windows native compositor | compositor mutex + condition variable | Phase/serial, latest diagnostics, viewport and wake state; no D3D call while held |
| Callback queues | local copies outside locks | Flutter frame callback, seek callback, failure callback, audio callback |

Renderer lock nesting remains:

```text
lifecycle_mutex_ -> state_mutex_ -> backend device mutex -> backend texture/target lock
```

Rules:

- Public APIs that mutate lifecycle or cross-thread resources enter through
  `lifecycle_mutex_`, then take `state_mutex_` only for short snapshots or state
  updates.
- `shutdown()` releases `state_mutex_` before joining the render thread.
- The render thread never takes `lifecycle_mutex_`; it coordinates with public
  APIs through atomics, queues, buffers, and `state_mutex_` snapshots.
- Backend GPU waits must not happen while holding platform texture publication
  locks.
- Flutter frame callbacks, seek callbacks, failure callbacks, and audio callbacks
  run after the renderer/backend locks have been released.
- `TrackPipelineManager::stop_all()` only runs after render-loop access to the
  track list is stopped or otherwise excluded.

## Windows Native Presentation

Windows native presentation is disabled in this restart branch. The shared
renderer keeps reserved D3D11/DX12 types, but no active Windows presentation
threading, DComp composition, or high-refresh contract is
claimed here. Reintroduce this section with the new lock order and validation
matrix when the Windows sandwich backend is rebuilt.

## macOS Metal / CVPixelBuffer Completion

The macOS backend uses the same render thread and `PresentationBackend`
interface. Platform-specific ownership is split as follows:

- Native owns the Metal device, command queue, `CVMetalTextureCache`, package
  upload, draw validation, presentation state, and refresh completion.
- Swift owns `CVPixelBuffer` ring creation/retention, stable target publication,
  and runner compositor notification.
- Swift installs or refreshes the target under a short texture lock, releases
  that lock, then calls
  `VPMacOSNativePlayerRequestNativeTargetFrameRefresh(...)`.
- Native waits on condition-style completion keyed by target generation, upload
  count, failure count, and last error. Success and failure callbacks wake
  waiters after renderer locks are released.

`VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(...)` is a compatibility
entry for copying information about the most recent native-target frame. It is
not the normal active refresh command.

## Renderer Component Boundaries

`Renderer` remains the public facade and lifecycle owner. `Renderer::Impl` is
the private composition root: it wires shared renderer controllers together,
owns the public lock surface, and adapts public API calls into smaller command
contexts. New code should keep policy in shared native components and keep
platform backends focused on presentation.

Current shared renderer ownership is split as follows:

- `RendererTrackController` is the compatibility facade over
  `RendererTrackRegistry`, `RendererTrackMutationController`, and
  `RendererTrackPresentationModel`.
- `RendererTrackRegistry` owns track storage, slot/file identity, generation
  allocation, and cached duration state.
- `RendererTrackMutationController` owns add/remove/recreate/seek/offset/decode
  pause mutations against existing track storage.
- `RendererTrackPresentationModel` builds immutable track snapshots, present
  decisions, paused-preview decisions, and per-track diagnostics.
- `RendererPresentCommandProcessor` owns paused redraw, layout redraw, draw
  snapshot submission, completion publication, and present diagnostics through
  an explicit `RendererPresentCommandContext`.
- `RendererRenderLoopCommandProcessor` owns render-thread cadence, preroll,
  paused preview, scheduler ticks, EOF settle, and deadline sleep through an
  explicit `RendererRenderLoopCommandContext`.
- `RendererPresentationController` owns backend/device/callback/capture storage
  and is the only shared renderer controller that submits to the platform
  `PresentationBackend`.
- `RendererLayoutState`, `RendererTimelineController`, `RendererEventBus`, and
  `PresentationMetricsStore` own layout revisions, playback/seek/loop state,
  native events, and presentation metrics respectively.

- Render loop / tick / present scheduling: shared renderer code.
- Track lifecycle, layout, seek, loop, EOF settle, and refresh completion: shared
  renderer/native player code.
- D3D11/DX12 target, hardware frame import, and Windows capture:
  reserved Windows presentation backend.
- Metal/CVPixelBuffer/IOSurface target, VideoToolbox frame preservation, Metal
  package upload, and macOS capture: macOS presentation backend.
- Analysis overlay raster/upload and future analysis IPC remain separate
  subsystem concerns; they should not introduce another playback scheduler.

Present/render-loop command contexts may contain hooks whose names end in
`_locked`. Those hooks require the caller to already hold `state_mutex_`.
Hooks that receive `std::unique_lock<std::mutex>&` may temporarily unlock during
long operations, but must preserve the documented renderer lock order when they
relock. Hooks that call platform callbacks or submit GPU/backend work must say
so at the hook boundary and must not run callbacks while renderer/backend locks
are held.

Before splitting another component, document whether it may take `state_mutex_`,
call platform callbacks, or touch backend GPU resources.

## Thread Communication

| Direction | Mechanism | Purpose |
| --- | --- | --- |
| Renderer -> DemuxThread | `SeekController` | Seek requests and stream flush |
| Renderer -> DecodeThread | seek notification / queue flush | Drop stale frames and restart preroll |
| DemuxThread -> DecodeThread | `PacketQueue` | Packet transfer |
| DecodeThread -> RenderThread | `TrackBuffer` | Decoded frame transfer |
| RenderThread -> platform runner | frame/failure callback | Texture invalidation and diagnostics |
| PacketQueue | EOF signal | Demux completion |
| PacketQueue | `abort()` | Stop blocked demux/decode work |

## Startup And Shutdown

```text
Startup:
  1. Create renderer/native player facade.
  2. Initialize platform PresentationBackend.
  3. Create TrackPipeline instances and queues.
  4. Start DemuxThread and DecodeThread per track.
  5. Wait for preroll or explicit ready state.
  6. Start RenderThread and begin presenting snapshots.

Shutdown:
  1. Stop RenderThread and wake refresh waiters.
  2. Stop DecodeThread and abort packet queues.
  3. Stop DemuxThread and close inputs.
  4. Release track buffers and decoder surfaces.
  5. Clear platform presentation targets and backend resources.
```

Platform runners may close windows while native playback is active. Shutdown
must wake macOS refresh waits and Windows texture acquisition paths before
destroying backend resources.
