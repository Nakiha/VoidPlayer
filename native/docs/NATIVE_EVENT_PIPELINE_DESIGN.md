# Native Event Pipeline Design

Status: Partially implemented. The initial `seekPreviewPresented` path is live;
the broader playback/buffering/device event set remains planned.

本文档定义 native 到 Dart 的事件通知管线。目标是把“native 已经知道的事实”
主动推给 Flutter，减少 Dart 侧对播放、seek、buffer 和渲染状态的轮询。

## Problem

当前主窗口有几类状态只能由 Dart 定时猜测或主动查询：

- seek 后 preview frame 何时真正上屏。
- 当前上屏帧的 PTS/DTS 和 track file id。
- buffer/playback/device/error 状态何时变化。
- analysis overlay 需要等到实际帧上屏后再按该帧生成 VACHUNK。

这会造成两个问题：

- Dart 用固定 timer 近似 native 状态，容易出现早一拍或晚一拍。
- 后续功能会继续添加 `currentXxx()` 查询，轮询路径越来越多。

## Goals

- 提供一条 native -> Dart 的低频状态事件通道。
- 让 seek preview 上屏事件成为 overlay refresh 的首要触发源。
- 事件由 native 事实驱动，不依赖 Dart timer 推测。
- renderer/decode/demux 线程不直接调用 Flutter `EventSink`。
- 事件协议可版本化、可测试、可向后兼容扩展。

## Non-Goals

- 不做每帧播放进度推送；timeline 进度仍由现有播放状态节奏控制。
- 不替代 `video_renderer` MethodChannel 的命令 API。
- 不承载 analysis 子窗口 IPC。
- 不在第一版做通用全局 event bus；先服务 native player/renderer 事件。

## Ownership

| Layer | Responsibility |
| --- | --- |
| `native/video_renderer` | 产生 renderer/player 事实事件，例如 seek preview 已上屏。 |
| `native/windows/player` | 为跨 renderer/playback 的事件补充 request id、track file id、状态原因。 |
| `windows/runner/video_renderer_plugin.*` | 持有 Flutter `EventChannel`、线程安全队列和平台线程派发。 |
| `lib/native_player/` | 解析事件 envelope，暴露 typed Dart stream。 |
| `lib/windows/main/` | 订阅事件并驱动 coordinator，例如 analysis overlay refresh。 |

## Channel Shape

第一版新增一个 EventChannel：

```text
video_renderer/events
```

MethodChannel 继续使用：

```text
video_renderer
```

事件 envelope 使用 `Map<String, Object?>`，所有事件共享稳定字段：

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

- `schemaVersion`: Dart 按 major schema 解析；未知 major 直接忽略。
- `sequence`: native 单调递增，方便日志和丢事件诊断。
- `type`: 字符串事件类型，Dart 侧转为 enum。
- `timestampUs`: native 单调时钟时间，不等同媒体 PTS。
- `requestId`: Dart 发起命令时传入；无对应命令时为 null。
- `trackFileId`: 对应 Dart track/file id；全局状态事件可为 null。
- `ptsUs`/`dtsUs`: 媒体时间，只有上屏帧类事件必须同时提供。

## Initial Event Types

| Type | Producer | Purpose |
| --- | --- | --- |
| `seekStarted` | Renderer seek path | 确认 native 已接收并开始处理 seek。 |
| `seekPreviewPresented` | Renderer paused preview present path | 精确 seek 后 preview frame 已实际上屏。 |
| `playbackStateChanged` | Player/playback controller | 播放、暂停、停止、EOF 状态变化。 |
| `bufferingStateChanged` | Track buffer / renderer | Ready、Buffering、Flushing 等状态变化。 |
| `deviceLost` | D3D11 backend / renderer | DXGI device lost，需要 UI 展示或重建。 |
| `trackError` | Demux/decode/render path | 单轨错误，包含 track/file id 和 reason。 |

第一轮实现只需要 `seekPreviewPresented` 即可消除 overlay seek timer；
其他事件按同一协议逐步补齐。

## Seek Contract

Dart 发起 seek 时传入可选 `requestId`：

```text
NativePlayerController.seek(ptsUs, requestId: nextSeekRequestId)
```

native 完成 paused preview 上屏后发送：

```text
seekPreviewPresented {
  requestId,
  trackFileId,
  ptsUs,
  dtsUs,
  targetPtsUs
}
```

语义约束：

- 事件必须在 preview frame 调用 native present 路径之后发送。
- `ptsUs` 与 `dtsUs` 来自实际上屏 frame，不允许用 target seek time 代替。
- 同一次 request 如果内部重试，只发送最后一次实际上屏事件。
- Dart 只响应最新 pending seek 的 request id；旧事件只记录日志。
- request id 缺失时，Dart 可按最新 seek fallback，但不得作为主路径。

## Threading

native 生产者可能位于 renderer、decode、demux 或 playback 线程。它们只允许调用
native event dispatcher：

```text
producer thread
  -> enqueue immutable event
  -> signal runner bridge through a message-only HWND
  -> platform thread drains queue
  -> Flutter EventSink.Success(map)
```

硬约束：

- 不在 renderer/decode/demux 线程直接调用 Flutter `EventSink`。
- 队列持有 plain data，不持有 AVFrame、D3D texture、Flutter 对象。
- 队列容量有限；低价值状态事件可合并，seek/error/device 事件不可静默合并。
- app shutdown 或 stream cancel 后，producer 仍可 enqueue，但 bridge 会直接丢弃。

## Backpressure

第一版队列容量建议 256。

处理策略：

- `seekPreviewPresented`、`trackError`、`deviceLost`：保留，队满时记录 warn 并丢最旧普通状态事件。
- `playbackStateChanged`、`bufferingStateChanged`：同类型同 track 可合并为最新状态。
- 不加入每帧事件，避免把事件通道变成进度 ticker。

## Dart API

`lib/native_player/` 暴露 typed stream：

```dart
Stream<NativePlayerEvent> get events;
```

推荐模型：

```text
NativePlayerEvent
  schemaVersion
  sequence
  timestampUs
  type

SeekPreviewPresentedEvent
  requestId
  trackFileId
  ptsUs
  dtsUs
  targetPtsUs
```

Dart 对未知事件类型保持忽略，不能 throw 到 stream 顶层导致订阅断开。

## Failure And Fallback

事件通道不可用时，功能应退化但不死锁：

- seek 命令仍通过 MethodChannel 完成。
- overlay refresh 保留短期 watchdog fallback，用于老 runner 或异常断线。
- fallback 只作为保护，不作为正常路径；日志需要区分 event path 和 timeout path。

## Testing

实现时至少覆盖：

- C++ event envelope 序列化单测。
- EventChannel subscribe/cancel 生命周期 smoke。
- seek 后收到 `seekPreviewPresented` 的 UI 自动化脚本。
- overlay seek 边界脚本：确认事件触发后能生成所需 VACHUNK 并 redraw。
- 日志中能看到 request id 从 Dart seek 贯穿到 native event。

推荐回归：

```bash
python dev.py test --native-only
python dev.py ui-test --build ui_tests/analysis/overlay_seek_boundary_vvc.csv ui_tests/analysis/overlay_seek_boundary_hevc_aq.csv
```

## Rollout

1. [x] 增加 renderer event callback 和 Windows runner EventChannel。
2. [x] 给 `seek()` 增加可选 `requestId`，保持旧调用兼容。
3. [x] renderer 在 paused preview present 后发送 `seekPreviewPresented`。
4. [x] Dart 新增 typed event stream。
5. [x] `MainWindowPlaybackCoordinator` 用事件替代固定 seek-settled timer 主路径。
6. [x] 保留 watchdog fallback。
7. [ ] 补齐 playback/buffering/device/error 等低频事件。
