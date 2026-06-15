# Threading Model

VoidPlayer native now uses one shared playback/render scheduler with platform
presentation backends. The threading contract is owned by shared native code;
Windows D3D11 and macOS Metal only differ once a `RendererDrawSnapshot` is handed
to a `PresentationBackend`.

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
 D3D11 shared texture             Metal/CVPixelBuffer/IOSurface
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
- macOS VideoToolbox preserves `CVPixelBuffer` frames for the renderer-owned
  Metal path when the codec/pixel format is supported, and otherwise falls back
  to explicit software or hwdownload packages.
- Does not call renderer public lifecycle APIs and does not access platform
  texture publication locks directly.

### Render Thread

- Owns playback cadence and deadline sleep.
- Reads the shared `Clock`, asks `RenderSink` for a `PresentDecision`, and builds
  `RendererDrawSnapshot` values.
- Calls `PresentationBackend::draw_frame()` for normal playback, paused redraw,
  layout refresh, seek refresh, step refresh, capture preparation, and EOF
  settling.
- During playback, video source selection remains PTS-driven, but display-link
  layout refresh may re-composite the last completed source frame. The render
  thread skips duplicate source updates and never queues redraws faster than
  display ticks.
- Publishes success/failure state and frame callbacks after releasing renderer
  locks.
- Does not call public lifecycle APIs and never joins itself.

### Platform Host / UI Thread

- Windows runner acquires the latest shared D3D11 texture handle and releases it
  through the Flutter texture descriptor callback.
- In `native-compositor-scrgb`, the Windows platform thread owns the serial/ACK
  state machine only. A dedicated composition thread consumes immutable Flutter
  BGRA and video FP16 leases, draws and presents the DComp swap chain, and keeps
  the latest successfully synchronized inputs leased until a newer generation
  replaces them or the compositor stops. This lets one input update re-composite
  against stable video, Flutter, and source-cache inputs without reacquiring a
  keyed-mutex frame that was already consumed.
- Source-cache publication stays on the renderer D3D11 device mutex. One render
  draw acquires every target in a bundle, renders all tracks, flushes, then
  publishes one generation. The composition thread acquires that bundle as one
  lease, never mixes generations, retains the latest successful bundle for
  projection-only redraws, and treats keyed-mutex timeout as backpressure rather
  than a compositor-wide failure.
- macOS runner owns Cocoa, sandbox file access, platform channels, Flutter
  texture registration, `CVPixelBuffer` lifecycle, and frame notification.
- macOS viewport pan/zoom submits only the latest layout intent. `CVDisplayLink`
  coalesces input and keeps a short idle grace. Native decides whether the tick
  updates a video source, composites the retained source cache, skips for ring
  pressure, or fails with a visible diagnostic.
- macOS seek/step/startup/paused/EOF refresh calls install or validate the
  renderer-owned target, release the Swift texture lock, then request native
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

## Windows D3D11 Serialization

The Windows backend serializes D3D11 immediate-context work under the backend
device mutex. This includes draw, copy, flush/fence wait, headless shared texture
publish, and any D3D11VA path that shares the immediate context.

The headless output path uses this order:

```text
device_mutex -> D3D11HeadlessOutput::texture_mutex()
```

`D3D11HeadlessOutput::*_locked()` methods require the caller to already hold the
texture mutex. GPU fence waits happen under the device mutex only, then the
texture mutex is reacquired briefly to swap the front buffer and collect the
Flutter callback. The callback runs outside both locks.

Flutter consumers obtain an AddRef'ed shared texture snapshot through
`acquire_shared_texture()`. They must not cache native texture pointers that were
not leased through that API.

The Windows DComp route uses independent producer/exporter devices and a
runner-owned final compositor device. On a matching adapter, shared BGRA/FP16
slots cross those boundaries only through NT handles, keyed mutex keys,
generation-stamped leases, and explicit release. On an output-adapter mismatch,
the producer renderer and Flutter exporter remain on their original adapter;
the composition thread recreates only the final D3D/DComp device on the output
adapter and bridges each immutable lease through row-major shared textures plus
GPU copies into output-local SRVs. The current bridge waits for producer copies
with a D3D11 event query; shared-fence capability is diagnosed separately and is
not part of the lock contract. Resize creates a new ring generation; an old
generation cannot be destroyed until every consumer lease is returned. Flutter
state ACKs are post-frame commits: activation publishes `active` after the
transparent viewport ACK, while fallback teardown is queued to the composition
thread after the restored-Texture ACK.

The platform thread receives `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE`, `WM_MOVE`,
`WM_EXITSIZEMOVE`, and `WM_DPICHANGED`, coalesces them with a short timer, then
runs the same display/presentation resolver used by diagnostics and track
mutations. The composition thread builds a candidate SDR or scRGB swap chain,
waits for video/source generations rendered with the new white level, migrates
the output device when the resolved output adapter changes, Presents the
candidate, and only then commits it to the DComp visual. Existing source/Flutter
leases remain valid during this transition; cross-adapter transport failure
falls back to producer-adapter native SDR before restoring Flutter Texture SDR.

Source-cache clear and signature replacement stop new acquisition immediately.
Any generation already leased by DComp remains in the retired set until release.
An unchanged-signature draw miss leaves the last complete bundle published;
partial source updates are cancelled before publication.

## macOS Metal / CVPixelBuffer Completion

The macOS backend uses the same render thread and `PresentationBackend`
interface. Platform-specific ownership is split as follows:

- Native owns the Metal device, command queue, `CVMetalTextureCache`, package
  upload, draw validation, presentation state, and refresh completion.
- Swift owns `CVPixelBuffer` creation/retention, Flutter texture registration,
  and `markTextureFrameAvailable` notifications.
- Swift installs or refreshes the target under a short texture lock, releases
  that lock, then calls
  `VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(...)`.
- Native waits on condition-style completion keyed by target generation, upload
  count, failure count, and last error. Success and failure callbacks wake
  waiters after renderer locks are released.

`VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(...)` is a compatibility
entry for copying information about the most recent renderer-owned frame. It is
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
- D3D11 shared texture, swap chain, D3D11VA texture upload, and Windows capture:
  Windows presentation backend.
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
