# Native Event Pipeline

本文档定义 native 到 Dart 的低频事实事件通道。事件管线用于把
native 已经确认的播放、seek、渲染或错误事实推给 Flutter，避免 Dart
用 timer 猜测 native 状态。

当前已落地的主事件是 `seekPreviewPresented`。更广的 playback、
buffering、device、track error 事件可以沿用同一 envelope 增量扩展。

## Channels

MethodChannel 继续承载命令：

```text
video_renderer
```

EventChannel 承载 native 事实事件：

```text
video_renderer/events
```

## Ownership

| Layer | Responsibility |
| --- | --- |
| `native/renderer` | 产生 renderer/player 事实事件，例如 seek preview 已上屏。 |
| platform native player bridge | 补充 request id、track file id、状态原因，并保持平台线程隔离。 |
| runner EventChannel bridge | 持有 Flutter `EventSink`、线程安全队列和平台线程派发。 |
| `lib/native_player/` | 解析事件 envelope，暴露 typed Dart stream。 |
| main-window coordinators | 订阅事件并驱动 overlay refresh 等 UI workflow。 |

Renderer、decode、demux 线程不得直接调用 Flutter `EventSink`。事件生产者只
能 enqueue plain-data envelope，由 runner 在平台线程 drain。

## Envelope

所有事件共享稳定字段：

```text
{
  "schemaVersion": 1,
  "sequence": 1234,
  "type": "seekPreviewPresented",
  "timestampUs": 707662203,
  "requestId": 42,
  "trackFileId": 7,
  "ptsUs": 2125000,
  "dtsUs": 2083000,
  "targetPtsUs": 2134000
}
```

字段规则：

| Field | Rule |
| --- | --- |
| `schemaVersion` | Dart 按 major schema 解析；未知 major 直接忽略。 |
| `sequence` | Native 单调递增，用于日志和丢事件诊断。 |
| `type` | 字符串事件类型，Dart 侧转为 enum。 |
| `timestampUs` | Native 单调时钟时间，不等同媒体 PTS。 |
| `requestId` | Dart 发起命令时传入；无对应命令时为 null。 |
| `trackFileId` | 对应 Dart track/file id；全局状态事件可为 null。 |
| `ptsUs` / `dtsUs` | 媒体时间；上屏帧类事件必须来自实际上屏帧。 |

## Event Types

| Type | Producer | Purpose |
| --- | --- | --- |
| `seekPreviewPresented` | Renderer paused preview present path | 精确 seek 后 preview frame 已实际上屏。 |
| `trackError` | Demux/decode/render path | 单轨错误，包含 track/file id 和 reason。 |

Additional low-frequency events use the same envelope instead of creating new
channels:

| Type | Purpose |
| --- | --- |
| `seekStarted` | Native 已接收并开始处理 seek。 |
| `playbackStateChanged` | 播放、暂停、停止、EOF 状态变化。 |
| `bufferingStateChanged` | Ready、Buffering、Flushing 等状态变化。 |
| `deviceLost` | GPU device lost，需要 UI 展示或重建。 |

## Seek Preview Contract

Dart 发起 seek 时传入可选 `requestId`：

```text
NativePlayerController.seek(ptsUs, requestId: nextSeekRequestId)
```

Native 完成 paused preview 上屏后发送 `seekPreviewPresented`。

Hard rules:

- 事件必须在 preview frame 调用 native present 路径之后发送。
- `ptsUs` 与 `dtsUs` 来自实际上屏 frame，不能用 target seek time 代替。
- 同一次 request 如果内部重试，只发送最后一次实际上屏事件。
- Dart 只响应最新 pending seek 的 request id；旧事件只记录日志。
- request id 缺失时，Dart 可按最新 seek fallback，但不得作为主路径。

## Backpressure

Event queues are bounded. The bridge may coalesce ordinary state updates, but
must not silently drop `seekPreviewPresented`, `trackError`, or `deviceLost`
without logging. The channel must never become a per-frame progress ticker.

## Failure Semantics

EventChannel unavailability must not block commands:

- seek still completes through MethodChannel;
- overlay refresh keeps a watchdog fallback for old runners or disconnected
  streams;
- logs distinguish event-path refresh from timeout fallback.

## Verification

Minimum coverage for event-pipeline changes:

```bash
python dev.py test --native-only
python dev.py ui-test --build ui_tests/analysis/overlay_seek_boundary_vvc.csv ui_tests/analysis/overlay_seek_boundary_hevc_aq.csv
```

macOS event bridge changes should also cover:

```bash
python dev.py mac-ui-test --build ui_tests/macos/native_seek_preview_event_smoke.csv
```
