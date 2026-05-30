# Analysis Overlay Refresh Design

Status: Implemented for the initial seek-driven overlay refresh path.

本文档定义主窗口 analysis overlay 在 seek、VACHUNK 生成和 native 绘制之间的刷新
策略。它是 [ANALYSIS_CACHE_OVERLAY.md](ANALYSIS_CACHE_OVERLAY.md) 的实施前设计补充。

## Problem

当前 overlay seek 刷新经历过多轮补丁：

- seek 后由 Dart 固定 timer 触发 `refreshOverlayForCurrentFrame()`。
- timer 触发时，实际 preview frame 可能还没上屏，或已经上屏但 Dart 估算到相邻帧。
- 64 帧 VACHUNK window 边界附近容易只生成一个窗口，最终 native 读到的上屏帧落在另一个窗口。
- native `AnalysisManager::read_vac2_overlay_frame()` 每次绘制会扫描 overlay chunk 目录。

目标不是让 seek 等分析，而是让分析在正确的 native 事件后排队执行，并让 native 读路径足够轻。

## Goals

- seek 和帧上屏不被 VACHUNK 生成阻塞。
- overlay refresh 由实际上屏帧事件触发，而不是固定延时推测。
- VAC2 frame matching 必须同时匹配 PTS 和 DTS。
- VACHUNK window 选择对边界 seek 宽容。
- native overlay frame 读取不在每次 draw 时反复扫描目录和打开所有 chunk。
- 文档化 fallback，使事件通道异常时仍可恢复显示。

## Non-Goals

- 不在 Dart 绘制 CU/MB。
- 不把所有 VACHUNK records 搬到 Dart。
- 不把 VACHUNK 生成放进 seek 同步路径。
- 不在第一版做每帧预生成全片 overlay chunk。

## Desired Flow

```text
Dart timeline/action seek
  -> NativePlayerController.seek(targetPtsUs, requestId)
  -> native renderer seek/preroll
  -> renderer presents paused preview frame
  -> native emits seekPreviewPresented(requestId, trackFileId, ptsUs, dtsUs)
  -> MainWindowPlaybackCoordinator accepts latest requestId
  -> MainWindowAnalysisCoordinator.refreshOverlayForPresentedFrame(ptsUs, dtsUs)
  -> VAC2 frame lookup requires PTS + DTS match
  -> ensure selected VACHUNK windows
  -> reload native overlay track
  -> applyLayout/overlay state update asks renderer to redraw
```

关键点：

- `seek()` 返回不代表 overlay 已准备好。
- `seekPreviewPresented` 代表可以安全地按实际上屏帧生成 overlay chunk。
- VACHUNK 生成进入现有 `SerialAnalysisGenerationQueue`，仍在后台排队。
- 生成完成后 reload overlay track，再请求 redraw。

## Dart State Machine

`MainWindowPlaybackCoordinator` 维护最新 pending seek：

```text
PendingSeek {
  requestId
  targetPtsUs
  createdAt
  completedByEvent
}
```

事件处理规则：

- 只接受 `requestId == latestPendingSeek.requestId` 的 `seekPreviewPresented`。
- 收到旧 request id 的事件时忽略，但保留 debug 日志。
- 收到事件后取消 seek-settled timer fallback。
- 如果 event stream 断开或超时，fallback 调用一次当前 `refreshOverlayForCurrentFrame()`。
- fallback 必须标记日志，便于区分正常 event path。

`NativePlayerController.currentPresentedFrame()` 保留，但定位变为：

- 用户显式查询当前帧。
- event pipeline 不可用时的 fallback。
- 测试和诊断工具。

它不应作为 overlay seek 刷新的常规轮询入口。

## Frame Matching

VAC2 frame lookup 使用实际呈现帧的 PTS/DTS：

```text
presented ptsUs + dtsUs
  -> VAC2 frame table
  -> require same PTS and same DTS
  -> frameIndex
```

约束：

- 不允许 PTS-only 命中作为正常路径，因为坏视频或 B-frame 重排场景可能撞 PTS。
- DTS 缺失时可以 fallback 到 summary estimate，但必须记录日志并降低置信度。
- overlay chunk window 以最终 `frameIndex` 为准，不以 target seek time 为准。

## VACHUNK Window Policy

当前固定 window size 为 64 frames。给定 `frameIndex`：

```text
currentWindow = floor(frameIndex / 64)
position = frameIndex % 64

always ensure currentWindow
if position < 16: ensure previousWindow
if position >= 48: ensure nextWindow
```

理由：

- 实际上屏帧和 Dart/native 查表可能在边界附近差一帧。
- 只等 current window 在 63/64、127/128 这类边界上过紧。
- 前后 1/4 触发相邻窗口，成本有限，但能覆盖 seek 边界抖动。

生成完成条件：

- 本次策略选中的所有 window 都完成或已经存在。
- 存在旧 generator revision 的 chunk 不视为完成。
- 任一 window 失败时保留视频上屏，只记录 overlay 缺失/失败状态。

## Native Overlay Chunk Index

native 绘制 path 应避免每次 draw 扫描：

```text
draw_analysis_overlay()
  -> AnalysisManager.read_overlay_frame(frameIndex)
  -> overlay chunk index lookup
  -> open selected VACHUNK only
  -> read frame records
```

建议在每个 overlay track manager 内维护：

```text
OverlayChunkIndexEntry {
  startFrame
  endFrame
  baseRevision
  generatorRevision
  path
}
```

索引刷新策略：

- `set_overlay_track()` 绑定 track 时扫描 `cache/<hash>/chunks/overlay` 并建立内存索引。
- 普通 draw path 只查内存索引和 decoded chunk cache；frame miss 直接跳过 overlay，不在上屏线程重扫目录。
- VACHUNK window 生成完成后由 Dart reload native overlay track，刷新索引并触发 redraw，覆盖“chunk 刚生成完成”的情况。
- 如果多个 chunk 覆盖同一 frame，选择最高 `generatorRevision`，再选择最高 `baseRevision`。
- `set_overlay_track()` 或 `clear_overlay_tracks()` 清空对应索引。
- 旧 revision chunk 可以留在磁盘，但不能优先于新 revision。

这样可以把热路径从“每帧遍历目录并打开多个文件”降到“查内存索引并打开一个文件”。

当前 macOS Metal 上屏路径还要求更严格：display-link viewport composite
可能高于视频 PTS 频率运行，因此 overlay 不应在每个 composite tick 重建
primitive。当前 contract 是：

- Dart 只在 track set 变化、目标 chunk 缺失或 chunk-ready 后刷新 native
  overlay track；普通 seek/layout tick 不重复 `set_overlay_track()`。
- native primitive package 按 session、track file id、frame、slot、视频尺寸和
  overlay config 缓存，坐标保持在 video space。
- Metal backend 再按 primitive package generation 缓存已打包的 GPU rect/line
  列表；pan/zoom/split 只更新 layout params，由 shader 投影到目标 buffer。
- CPU overlay fallback 只用于 GPU overlay 失败后的可见 fallback，不能作为
  正常高刷新路径。

## Redraw Contract

overlay refresh 完成后只做两件事：

- 更新 native overlay track/session，使 renderer 能读到新 chunk。
- 触发一次 layout/redraw。

不做：

- 不 replay seek。
- 不阻塞用户继续拖 timeline。
- 不假设当前上屏帧仍然等于开始生成时的帧；如果期间又发生新 seek，旧 request id 的生成结果不主动抢屏。

## Logging

建议日志字段：

```text
[Analysis] overlay_refresh requestId=42 path=event pts=2125000 dts=2083000 frame=128 windows=64-127,128-191
[Analysis] overlay_refresh requestId=42 path=fallback targetPts=2134000 frame=...
[Analysis] overlay_chunk_index reload entries=3 reason=miss frame=128
```

日志要能回答：

- 这次 refresh 是 event 触发还是 fallback 触发。
- 使用的是 target time、presented PTS/DTS，还是 summary estimate。
- 选择了哪些 VACHUNK window。
- native 读到的是哪个 revision 的 chunk。

## Testing

必测场景：

- seek 到 64-frame 边界附近，overlay 最终上屏。
- 连续快速 seek，旧 request id 的 overlay 不抢最新 seek。
- 清空 overlay chunks 但保留 `base.vac`，seek 后按需生成并上屏。
- 目录中同时存在旧/new generator revision，native 选择新 revision。

推荐命令：

```bash
python dev.py test --native-only
python dev.py ui-test --build ui_tests/analysis/overlay_seek_boundary_vvc.csv ui_tests/analysis/overlay_seek_boundary_hevc_aq.csv
```

如果实现触及 timeline 真实点击路径，补跑：

```bash
python dev.py ui-test --build ui_tests/timeline/h265_timeline_click_visual_regression.csv
```

## Rollout

1. [x] 先实现 native event pipeline 的 `seekPreviewPresented`。
2. [x] Dart seek 传 `requestId`，playback coordinator 订阅 event stream。
3. [x] overlay refresh 改为 event-first、timeout fallback。
4. [x] native `AnalysisManager` 增加 overlay chunk index。
5. [x] 更新/执行 UI tests，让边界 seek 覆盖 event path。
6. [ ] 稳定后继续收窄不再需要的查询式入口。
