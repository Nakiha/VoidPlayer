# 线程模型

## 线程角色

每路视频轨道拥有独立的 Demux + Decode 线程对，渲染线程为全局单例。

```
┌──────────────────────────────────────────────────────────┐
│                    Renderer (主线程)                       │
│  initialize() / shutdown() / play() / seek() / ...       │
└──────────────────────┬───────────────────────────────────┘
                       │ 启动/停止
       ┌───────────────┼───────────────┐
       ▼               ▼               ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  Track 0     │ │  Track 1     │ │  Track N     │
│              │ │              │ │              │
│ DemuxThread  │ │ DemuxThread  │ │ DemuxThread  │
│ DecodeThread │ │ DecodeThread │ │ DecodeThread │
└──────────────┘ └──────────────┘ └──────────────┘
                       │
                       ▼
              ┌──────────────┐
              │ RenderThread │  单例，独占 D3D11 Context
              └──────────────┘
```

## 三类线程

### Demux 线程（每轨道一个）

- 从视频文件读取 AVPacket
- 过滤非视频 stream，PTS 转微秒
- 写入 PacketQueue（阻塞式有界队列）
- 轮询 SeekController 处理 pending seek

### Decode 线程（每轨道一个）

- 从 PacketQueue 消费 AVPacket
- FFmpeg 解码（D3D11VA 硬解 / 软解）
- FrameConverter 统一输出 TextureFrame
- 写入 TrackBuffer（BidiRingBuffer）
- 硬解时通过 `device_mutex_` 序列化 D3D11 immediate context 访问

### Render 线程（单例）

- 独占 D3D11 Immediate Context
- 从 Clock 获取 current_pts_us
- RenderSink 评估上屏决策
- 从各 TrackBuffer peek 帧
- PTS 对齐 → 合成 → Present
- Deadline-based sleep 避免漂移

## 渲染循环

```
while (running) {
    current_pts = clock_.current_pts_us();
    decision = render_sink_.evaluate();   // 对齐所有轨道

    if (decision.should_present) {
        composite(decision.frames);       // 着色器合成
        device_.present(sync_interval);   // vsync
    }

    // Deadline sleep: 计算下一帧的绝对 PTS，sleep 到该时刻
    next_pts = current_pts + frame_duration / speed;
    sleep_until(next_pts);
}
```

Deadline-based sleep 保证长时间播放无累积漂移。

## 锁策略

| 资源 | 保护方式 | 持锁时间 |
|------|---------|---------|
| Renderer lifecycle | `lifecycle_mutex_` | public API 入口、render thread join、headless texture snapshot |
| Renderer state | `state_mutex_` | tracks/layout/background/last decision/EOF/seek policy state |
| PacketQueue | mutex + condvar | push/pop 瞬间 |
| BidiRingBuffer | mutex | push/peek/advance 瞬间 |
| Clock | mutable mutex | 查询/更新瞬间 |
| SeekController | mutex + atomic | request/take 瞬间 |
| D3D11 Context | `device_mutex_` (`recursive_mutex`) | render draw/present、headless resize/publish、D3D11VA decode 使用 immediate context |
| Headless shared texture | texture_mutex | shared handle 查询、front/back 索引切换、resize、capture |
| TrackBuffer state | mutex | 状态变更瞬间 |

### Renderer 锁顺序

Renderer 内部允许的嵌套顺序是：

```text
lifecycle_mutex_ -> state_mutex_ -> device_mutex_ -> texture_mutex()
```

规则：

- Public API 入口如果会改变生命周期或跨线程资源，先拿 `lifecycle_mutex_`，再视需要短暂拿 `state_mutex_`。
- `shutdown()` 在释放 `state_mutex_` 后 join render thread；不能持有 `state_mutex_` 等待线程退出。
- Render thread 不拿 `lifecycle_mutex_`。它只通过 atomics、`state_mutex_` 快照和队列/buffer API 与外部 API 协调。
- D3D11 draw/present、headless resize/publish、GPU fence wait 只在 `device_mutex_` 下执行；不要在持有 `texture_mutex()` 时等待 GPU。
- Headless 输出路径的锁顺序固定为 `device_mutex_ -> texture_mutex()`。`D3D11HeadlessOutput::*_locked()` 方法要求调用方已持有 `texture_mutex()`；不要在这些方法内部再反向获取 `device_mutex_`。
- `Renderer::draw_headless_and_publish()` 会在内部短暂获取 `texture_mutex()`，调用方必须只持有 `device_mutex_`，不能预先持有 `texture_mutex()`。
- Callback（Flutter frame callback、seek callback、audio callback）不能在持有 `state_mutex_`、`device_mutex_` 或 `texture_mutex()` 时执行。需要 callback 时先复制/取出，再在锁外调用。
- `TrackPipelineManager::stop_all()` 只能在 render thread 已停或调用方已经保证没有 render-loop 并发访问 `tracks_` 时执行。

Headless publish 的同步契约：

- Native producer 在 `texture_mutex` 下选择 back buffer RTV，然后释放 texture lock 后执行 draw。
- GPU fence / idle wait 只在 `device_mutex` 下执行，不持有 `texture_mutex`，避免 Flutter texture acquire、resize、callback 更新被最长 100ms 的等待阻塞。
- fence 完成后再短暂持有 `texture_mutex` 切换 front buffer 并取出 callback；callback 在锁外执行。
- Flutter consumer 通过 `acquire_shared_texture()` 一次性拿到 AddRef 后的 texture 与 shared handle，release 由 Windows runner 的 `FlutterDesktopGpuSurfaceDescriptor::release_callback` 归还。
- `GetSharedHandle()` 失败是初始化/resize hard failure，不能发布空 handle。

## Renderer 调用线程契约

| 调用方 | 允许调用 / 访问 | 禁止事项 |
| --- | --- | --- |
| UI / FFI / Python host thread | `initialize`、`shutdown`、`play`、`pause`、`seek`、`set_speed`、track add/remove、layout/background、resize、texture acquire、stats/query API | 不要持有宿主 callback lock 后同步调用会等待 native callback 的 API；同一 player 的 C FFI API 通过 shared handle lease 保护句柄生命周期，destroy 使用独占 gate |
| Render thread | `render_loop`、`do_resize`、`present_frame`、`draw_frame`、headless publish、paused redraw、EOF settling | 不调用 public lifecycle API；不 join 自己；不执行 Flutter callback while holding renderer locks |
| Demux thread | 写 `PacketQueue`，消费 `SeekController`，通过注册 seek callback 通知 decode/audio | 不直接访问 `Renderer::tracks_`、layout、D3D resources |
| Decode thread | 消费 `PacketQueue`，写 `TrackBuffer`，硬解路径持 `device_mutex_` 使用 D3D11VA immediate context | 不访问 headless texture mutex；不调用 Renderer public API |
| Audio decode / callback path | 通过 `AudioCoordinator` 消费 audio packet queue，响应 seek/pause/speed state | 不访问 D3D resources 或 render layout state |
| Flutter texture consumer | 只通过 `acquire_shared_texture()` 获取 AddRef 后的 texture/handle snapshot | 不缓存未 AddRef 的 native texture pointer |

## Renderer 拆分边界

`Renderer` 仍是 native 播放器的 facade 和生命周期所有者。新增职责时优先放入已有组件；确实需要拆分时按下列边界切：

- Render loop / tick / present scheduling: 从 `render_loop()`、`present_frame()` 外提为 `RenderLoopController`，但 D3D draw 仍需遵守 `device_mutex_`。
- Layout validation / order / viewport math: `LayoutController` 已接管 file-id order 与 shader slot-order 翻译；后续再从 `display_pixel_size_for_layout_locked()`、`update_track_geometry_from_decision_locked()` 外提 viewport math。
- Analysis overlay cache + CPU raster + D3D upload: 从 `draw_analysis_overlay()`、`ensure_analysis_overlay_texture()` 外提为 `AnalysisOverlayRenderer`。
- Device loss terminal/recreate policy: 从 `enter_terminal_device_lost_locked()` 和 poll sites 外提为 `DeviceLossPolicy`。
- Front-buffer capture and snapshot helpers: `FrameCaptureService` 负责 headless front-buffer capture 的 `device_mutex_ -> texture_mutex()` 锁编排，`Renderer::capture_front_buffer()` 只保留生命周期门禁。

拆分前必须先写下新组件的锁所有权：组件是否能拿 `state_mutex_`、是否能调用 callback、是否能触碰 D3D immediate context。

## 线程间通信

| 通信方向 | 机制 | 用途 |
|---------|------|------|
| Renderer → DemuxThread | SeekController | 下发 seek 请求 |
| Renderer → DecodeThread | notify_seek() | 通知 seek，丢弃旧帧 |
| DemuxThread → DecodeThread | PacketQueue | AVPacket 传递 |
| DecodeThread → RenderThread | TrackBuffer | TextureFrame 传递 |
| PacketQueue | EOF signal | Demux 结束通知 |
| PacketQueue | abort() | 强制停止 |

## 启停顺序

```
启动: initialize()
  1. D3D11Device::initialize()      # GPU 初始化
  2. 各 TrackPipeline 构建          # Queue + Buffer
  3. DemuxThread::start()           # 开始填充 PacketQueue
  4. DecodeThread::start()          # 等待 Preroll 完成
  5. RenderThread 启动              # 开始上屏循环

停止: shutdown()
  1. RenderThread stop              # 退出渲染循环
  2. DecodeThread::stop()           # 停止解码
  3. PacketQueue::abort()           # 解除阻塞
  4. DemuxThread::stop()            # 停止读取
  5. D3D11Device::shutdown()        # 释放 GPU 资源
```
