# Native Compositor Convergence Plan

本文档记录 Windows DComp native compositor 与 macOS WGPU retained
compositor 的收敛方向。目标不是把平台代码强行合并，而是让两端共享同一套
compose 语义、状态边界、诊断口径和回归标准。

## Goal

VoidPlayer 的 native compositor 路线应该收敛为一个 thin-runner model：

```text
Dart/shared viewport state
  -> platform runner bridge
  -> platform native compositor backend
  -> platform present target
```

平台 runner 只负责窗口、平台 surface/layer 生命周期、MethodChannel/EventChannel
桥接、平台 display cadence 接入和诊断汇总。视频/source、Flutter UI surface、
projection、overlay 等输入必须作为 ready state 被 compositor 消费；runner 不应
拥有第二套几何计算、第二套 source package 发布规则或第二套上屏节奏。

## Current State

### Windows

Windows 已经更接近 thin runner：

```text
Dart viewport/projection
  -> windows/runner video_renderer_plugin
  -> WindowsNativeCompositor
  -> DComp/DXGI/D3D target
```

Windows runner 的主要职责是：

- 初始化和维护窗口、DComp/DXGI target、Flutter surface export ACK/serial。
- 把 Dart 发布的 viewport、source projection、Flutter UI surface 状态传给
  `WindowsNativeCompositor`。
- 暴露 native compositor state 和 diagnostics。

Windows runner 不应在 native compositor active 时自行捕获窗口、裁洞、使用
color-key，或绕过 compositor 生成另一条上屏路径。

### macOS

macOS WGPU retained path 的方向相同，但 runner 仍明显更厚：

```text
Dart viewport/projection
  -> MacOSVideoRendererBridge
  -> Flutter export lease / source-provider / display-link scheduler
  -> native wgpu-metal compositor
  -> CAMetalLayer
```

macOS runner 目前仍承担较多实现细节：

- `RendererOwnedCompositeDirty` dirty-bit 调度。
- `CVDisplayLink` tick 与 retained composite in-flight 管理。
- Flutter surface export acquire/release lease 管理。
- source-provider subscribe、ready-state、topology transaction。
- macOS-specific retained diagnostics。

这些职责有一部分必须留在平台层，例如 `CAMetalLayer` 和 `CVDisplayLink` glue；
但 source ready 语义、projection 几何、producer/consumer contract 和诊断口径应继续
向 shared/platform-neutral 层收敛。

## Target Contract

两端最终应实现同一组 contract。

### Geometry Authority

Dart/shared viewport code 是 source projection 的唯一几何权威。

- viewport rect、split/order、pan/zoom、per-source display transform 都从同一份
  `PresentationSourceProjection` 或等价结构进入 native backend。
- 平台 runner 禁止根据 window bounds、content view bounds、surface size 或
  target drawable size 重新推导 source projection。
- 平台 target 可以知道 drawable/hole 尺寸，但只能用于最终 present target mapping，
  不能改变 source projection 语义。

### Producer Model

所有输入都是 producer：

- video frame / source package
- Flutter UI export surface
- source projection
- viewport/target resize
- native overlay or analysis overlay state

producer 只能发布 latest ready state 和 dirty bit。producer 不能直接 submit
composite，不能阻塞 display tick，也不能发布半成品 package。

### Ready State

native compositor 每次 compose 只读取 ready state：

- `videoReady`
- `sourceReady`
- `flutterReady`
- `projectionReady`
- `targetReady`

如果某个 producer 正在准备新状态，display tick 应复用上一代 ready state。除了首帧
没有任何可用状态的情况，display tick 不应 blank、不应等待 decode/source bake/player
lock，也不应调用旧 frame refresh。

### Color Domain Ownership

每个 compositor input 必须声明自己的颜色域和合成归属：

- video/source texture: SDR BT.709、PQ/HLG HDR、platform linear scRGB/EDR 等。
- Flutter UI export surface: SDR sRGB premultiplied alpha，除非 Flutter engine 明确
  导出其它颜色域。
- native overlay / analysis overlay: SDR UI domain 或显式 video domain，不能隐式继承
  present target。

HDR/EDR target active 时，compositor 必须可诊断 Flutter UI 的处理方式：

- `system-managed`: Flutter UI 保持独立 SDR surface/layer，由 OS window compositor
  完成 SDR/HDR composition 和 SDR white/system calibration。
- `native-shader`: Flutter UI 被 native compositor shader sample 后写入 HDR/EDR target；
  此时应用必须承担 SDR reference white、transfer、premultiplied alpha 和 clamp 行为的
  验证责任。

Windows 的长期目标应优先让 HDR video/source 与 SDR Flutter UI 作为独立 DComp visual
参与系统合成；如果某阶段仍使用 `native-shader` 路径，必须通过 diagnostics 明确暴露
`FlutterSurfaceColorDomain`、`FlutterSurfaceCompositionOwner`、`FlutterSurfaceTargetDomain`
和是否 `CompositedIntoHDRTarget`。macOS EDR 路径当前属于 app-owned compositor，
因此必须继续保留 SDR UI/background/reference-white 的确定性验证。

当前 Windows Auto 产品策略在缺少 system-managed SDR Flutter UI + HDR video visual
拓扑前，必须把 HDR track on HDR output 降级到 native SDR，并用
`hdr-ui-composition-unsupported` 诊断说明。强制 `native-compositor-scrgb` 只能作为
诊断/实验路径保留，不能作为 Auto HDR 正确性证据。

### Single Compose Entry

唯一允许提交 retained/native composite 的入口是平台 display cadence：

- Windows: DComp/DXGI compositor cadence or compositor thread cadence.
- macOS: `CVDisplayLink` / retained `CAMetalLayer` cadence.

每个 tick 最多提交一次 composite。GPU in-flight 满时跳过本 tick 并保留 latest state，
不排 generation 队列，不让 producer cadence 淹没 display cadence。

### Transactional Topology

add track、remove track、seek、step、loop/EOF preview 等拓扑或时间跳变必须走统一
ready commit：

- 旧完整 source package 保留到新 topology ready。
- 新 topology 只有在 required visible slots 都有 drawable source 后才 publish。
- 失败时 fail closed，返回 missing slots/file ids/revision，不发布 blank package。
- Dart track model 和 viewport layout 不应长期领先 native source package。

## Convergence Phases

### Phase 1: Stop Divergence

Status: in progress.

- 删除平台 runner 中第二套 source projection 计算。
- 禁止 WGPU/native compositor active 时通过旧 target ring 或 frame refresh 驱动播放态
  上屏。
- 自动化覆盖 single-track playback、source projection、seek 和 add-track 中间态。

Validation focus:

- `rendererOwnedCompositeProducerSubmitCount == 0` during compositor windows.
- No sustained `install_target_ring` or `MacOSFrameRefresh timeout` in WGPU playback
  hot path.
- Single source projection keeps `displayOffsetY == 0` and `invDisplaySizeY == 1`
  when viewport geometry requires no vertical letterbox.

### Phase 2: Shared Projection Contract

Move projection data shape and validation closer to shared code.

- Treat `PresentationSourceProjection` as the cross-platform ABI concept.
- Add shared tests for single track, split track, pan/zoom, per-track ordering, and
  viewport resize.
- Keep platform-specific conversion thin and mechanical.

Expected outcome:

- Windows and macOS receive equivalent projection payloads for equivalent Dart
  viewport state.
- No platform runner computes projection from platform window/surface size.

### Phase 3: Shared Source Ready-State Semantics

Extract source-provider lifecycle into a shared contract.

- Define source package revisions, topology revisions, required/drawn/missing masks,
  and publish/consume counters in platform-neutral terms.
- Make seek/add-track/remove-track use the same ready commit semantics on both
  platforms.
- Keep platform texture/package creation backend-specific.

Expected outcome:

- add-track transitions from old complete topology directly to new complete topology.
- seek never depends on a platform-specific refresh workaround to push the first
  post-seek frame.
- incomplete package suppression is tested below UI automation where possible.

### Phase 4: Shared Compose Scheduler Contract

Unify scheduler semantics without forcing one implementation.

- Define dirty bit names and producer rules once.
- Define max in-flight behavior and latest-only queue semantics once.
- Keep Windows compositor thread and macOS display-link implementation
  platform-specific.

Expected outcome:

- diagnostics can answer the same questions on both platforms:
  - who produced a state?
  - was it ready?
  - when was it acquired?
  - when was it consumed?
  - did display cadence skip because GPU was in-flight?

### Phase 5: macOS Runner Thinning

Move macOS-specific retained state machine out of `MacOSVideoRendererBridge` into
smaller owned components.

Candidate boundaries:

- `MacOSRendererOwnedLayerHost`: `CAMetalLayer`, drawable, EDR/SDR target policy.
- `MacOSFlutterSurfaceLeaseCoordinator`: export acquire/release, retired leases.
- `MacOSSourceProviderCoordinator`: source topology ready transaction.
- `MacOSRetainedComposeScheduler`: dirty bits, display-link, in-flight accounting.

This phase is a cleanup only after the semantics are stable. Do not introduce new
async layers just to make files smaller.

## Diagnostics Contract

Keep platform diagnostics, but add or preserve platform-neutral aliases for common
questions:

| Question | Cross-platform diagnostic direction |
| --- | --- |
| Is native compositor active? | `nativeCompositorActive`, `nativeCompositorMode` |
| Is runner layer / native target active? | `nativeCompositorRunnerLayerActive` |
| Are producers directly submitting? | `nativeCompositorProducerSubmitCount` |
| What is display cadence? | `nativeCompositorPresentHz`, `nativeCompositorHostIntervalP95Ms` |
| How expensive is compose? | `nativeCompositorComposeP95Ms`, backend-specific submit/completion p95 |
| Is source ready complete? | `sourceTopologyRevision`, `sourceRequiredMask`, `sourceDrawnMask`, `sourceMissingMask` |
| Is source publish consumed? | `sourcePublishConsumeRatioX1000` |
| Is Flutter UI consumed? | `flutterPublishAcquireRatioX1000`, `flutterAcquireConsumeRatioX1000` |
| Who owns Flutter UI SDR/HDR composition? | `flutterSurfaceColorDomain`, `flutterSurfaceCompositionOwner`, `flutterSurfaceTargetDomain`, `flutterSurfaceCompositedIntoHDRTarget` |
| Where did SDR white come from? | `sdrWhiteLevelSource`, `sdrWhiteLevelMilliNits`, `sdrWhiteScaleX1000` |

Platform-prefixed fields such as `windowsDComp*` and `rendererOwned*` can remain,
but tests should prefer cross-platform aliases once they exist.

## Non-Goals

- Do not move Flutter overlay/quick mark editing into native compositor as a shortcut.
- Do not add timers, debounce windows, retry budgets, or deeper rings to hide
  incomplete source state.
- Do not reintroduce old FlutterTexture/ring presentation as a normal native
  compositor fallback.
- Do not make runner compute source geometry from platform window/surface size.
- Do not treat thin runner as permission to bake SDR Flutter UI into an HDR/EDR
  video target without an explicit color-domain contract and diagnostics.
- Do not force Windows and macOS to share platform glue code where APIs differ
  naturally.

## Review Checklist

For any future native compositor PR:

- Does the change preserve one geometry authority?
- Can every producer path be described as latest ready state plus dirty bit?
- Can display tick compose without waiting on decode, source bake, Flutter export,
  player locks, or old frame refresh?
- Does every input declare its color domain, and is Flutter UI composition
  system-managed or explicitly app-mapped with validation?
- Are seek and topology changes transactional rather than half-published?
- Are diagnostics sufficient to identify producer, acquire, consume, compose, and
  present stages?
- Does the validation include the platform-specific smoke plus at least one
  lower-level regression where the bug can be represented without UI automation?
