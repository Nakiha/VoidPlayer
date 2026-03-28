# C++ 视频渲染器设计文档

## 概述

基于 FFmpeg demux + D3D11VA 硬解 / 软解的多轨道视频渲染器。支持：
- 多路视频同时解码
- 帧级 PTS 对齐同步上屏
- 逐帧前进/回退
- I 帧 Seek / 精确 Seek
- 倍速播放

## 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Native 模块 (C++ DLL)                             │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        Render Thread                                 │   │
│  │  - 独占 D3D11 Immediate Context                                      │   │
│  │  - 时钟管理 + 上屏决策                                                │   │
│  │  - 从各 Track Sink Buffer peek 帧                                    │   │
│  │  - PTS 对齐 / 合成 / Present                                         │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                      ▲                                      │
│                                      │ TextureFrame                         │
│                                      │                                      │
│  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────┐  │
│  │      Track 0         │  │      Track 1         │  │    Track N       │  │
│  │  ┌────────────────┐  │  │  ┌────────────────┐  │  │                  │  │
│  │  │ BidiRingBuffer │  │  │  │ BidiRingBuffer │  │  │      ...         │  │
│  │  │  (纹理队列)     │  │  │  │  (纹理队列)     │  │  │                  │  │
│  │  └───────▲────────┘  │  │  └───────▲────────┘  │  │                  │  │
│  │          │           │  │          │           │  │                  │  │
│  │  ┌───────┴────────┐  │  │  ┌───────┴────────┐  │  │                  │  │
│  │  │ Decode Thread  │  │  │  │ Decode Thread  │  │  │                  │  │
│  │  └───────▲────────┘  │  │  └───────▲────────┘  │  │                  │  │
│  │          │           │  │          │           │  │                  │  │
│  │  ┌───────┴────────┐  │  │  ┌───────┴────────┐  │  │                  │  │
│  │  │  Packet Queue  │  │  │  │  Packet Queue  │  │  │                  │  │
│  │  └───────▲────────┘  │  │  └───────▲────────┘  │  │                  │  │
│  │          │           │  │          │           │  │                  │  │
│  │  ┌───────┴────────┐  │  │  ┌───────┴────────┐  │  │                  │  │
│  │  │ Demux Thread   │  │  │  │ Demux Thread   │  │  │                  │  │
│  │  │ (文件读取)      │  │  │  │ (文件读取)      │  │  │                  │  │
│  │  └────────────────┘  │  │  └────────────────┘  │  │                  │  │
│  └──────────────────────┘  └──────────────────────┘  └──────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
                            ┌─────────────────┐
                            │    FFmpeg DLL   │
                            │  (libs/ffmpeg)  │
                            └─────────────────┘
```

## 时间系统

### PTS 精度

所有 PTS 统一使用 **微秒 (μs)** 单位，避免浮点精度问题：

```
1 秒 = 1,000,000 μs
1 帧 @ 30fps = 33,333 μs
1 帧 @ 60fps = 16,666 μs
```

FFmpeg 时间基转换（在 Demux 层完成）：

```
AVStream.time_base (如 1/30000) → 微秒 (int64_t)
pts_us = av_rescale_q(pts, stream->time_base, {1, 1000000})
```

### 时钟管理

渲染线程维护一个主时钟：

```
┌────────────────────────────────────────────────────────────────┐
│                        Clock Manager                            │
│                                                                │
│  base_time_us     - 播放起始时的系统时间（微秒）                 │
│  base_pts_us      - 起始时的视频 PTS（微秒）                     │
│  speed            - 播放速度倍率（1.0 = 正常，2.0 = 2倍速）      │
│  paused           - 是否暂停                                    │
│  pause_time_us    - 暂停时的系统时间                             │
│                                                                │
│  current_pts_us = base_pts_us + (now - base_time_us) * speed   │
│                   （非暂停时）                                   │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

### 时钟操作

| 操作 | 行为 |
|-----|------|
| 播放 | `base_time_us = now`, `paused = false` |
| 暂停 | `pause_time_us = now`, `paused = true` |
| 恢复 | `base_time_us += (now - pause_time_us)`, `paused = false` |
| Seek | `base_pts_us = target`, `base_time_us = now` |
| 倍速 | 更新 `speed`，保持 `current_pts_us` 不变（调整 `base_time_us`） |

倍速切换时保持当前 PTS 不跳变：

```
new_base_time = now - (current_pts_us - base_pts_us) / new_speed
```

## 上屏决策

### 渲染循环

```
┌─────────────────────────────────────────────────────────────────┐
│                        Render Loop                               │
│                                                                 │
│  while (running) {                                              │
│      now_us = get_system_time_us()                              │
│      current_pts_us = calculate_current_pts(now_us)             │
│                                                                 │
│      if (should_present(current_pts_us)) {                      │
│          frames = align_all_tracks(current_pts_us)              │
│          composite_and_present(frames)                          │
│      }                                                          │
│                                                                 │
│      sleep_until_next_frame()                                   │
│  }                                                              │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 上屏判定逻辑

```
should_present(current_pts_us):

    对于每个 Track:
        frame = track.peek()  // 当前帧
        next_frame = track.peek(+1)  // 下一帧

        if (frame.pts_us + frame.duration_us <= current_pts_us) {
            // 当前帧已过期
            track.advance()
            continue
        }

        if (frame.pts_us <= current_pts_us && current_pts_us < frame.pts_us + frame.duration_us) {
            // 当前帧在显示时间内
            return true
        }

        if (frame.pts_us > current_pts_us) {
            // 当前帧还未到显示时间
            return false (等待)
        }

    return true (所有 track 都准备好)
```

### 多轨道 PTS 对齐

```
align_all_tracks(current_pts_us):

    frames_to_render = []

    for track in tracks:
        // 丢弃过期帧
        while (track.peek().pts_us + track.peek().duration_us < current_pts_us) {
            track.advance()
        }

        // 检查当前帧是否在时间窗口内
        frame = track.peek()

        if (abs(frame.pts_us - current_pts_us) <= PTS_TOLERANCE_US) {
            // 同步良好，选中此帧
            frames_to_render.append(frame)
        } else if (frame.pts_us > current_pts_us) {
            // 帧还未到，等待
            // 可以选择：显示上一帧 或 留空
            frames_to_render.append(track.peek(-1) ?: BLACK_FRAME)
        } else {
            // 帧已过期但未丢弃（异常情况）
            frames_to_render.append(frame)
            track.advance()
        }

    return frames_to_render
```

### PTS 同步容差

```
PTS_TOLERANCE_US = 5000  // 5ms 容差

原因：
- 不同视频源可能有微小的时间基差异
- 编码器可能产生不精确的 PTS
- 5ms 对于人眼不可察觉
```

### 帧间隔与倍速

```
frame_interval_us = 16666 / speed  // 基础 60fps 刷新，倍速时缩短

speed = 1.0  →  interval = 16.67ms
speed = 2.0  →  interval = 8.33ms
speed = 0.5  →  interval = 33.33ms
```

实际上屏间隔由视频帧率决定：

```
target_frame_duration_us = track[0].peek().duration_us
sleep_time_us = target_frame_duration_us / speed
```

## 线程模型

### 1. Demux 线程（每路视频一个）

**职责**：
- 从文件读取 AVPacket
- 过滤非视频包（直接丢弃）
- PTS 转换为微秒
- 填充 Packet Queue

**输入**：视频文件路径
**输出**：Packet Queue（带微秒 PTS 的 AVPacket）

**过滤逻辑**：
- 仅处理 `stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO`
- 非 video stream 的 packet 直接 `av_packet_unref` 丢弃

### 2. Decode 线程（每路视频一个）

**职责**：
- 从 Packet Queue 消费 AVPacket
- 调用 FFmpeg 解码（D3D11VA 硬解 / 软解）
- 格式转换统一为 RGBA 纹理
- 填充 BidiRingBuffer

**输入**：Packet Queue
**输出**：BidiRingBuffer（TextureFrame）

**硬解 vs 软解**：

| | 硬解 D3D11VA | 软解 |
|---|-------------|------|
| 输出格式 | `AV_PIX_FMT_D3D11` | `AV_PIX_FMT_YUV420P` 等 |
| 纹理来源 | 从 AVFrame 提取引用 | 创建并 Upload |
| 缓冲深度 | 2 帧 | 4 帧 |

### 3. Render 线程（单例）

**职责**：
- 独占 D3D11 Immediate Context
- 时钟管理（微秒精度）
- 上屏决策
- 从各 Track 的 BidiRingBuffer peek 帧
- PTS 对齐同步
- 执行着色器合成
- Present 上屏

**输入**：各 Track 的 BidiRingBuffer
**输出**：SwapChain Present

## 队列设计

### Packet Queue（Demux → Decode）

```
┌────────────────────────────────────────────────────┐
│  [pkt0][pkt1][pkt2] ... [pkt49] ... [pkt99]       │
│                                                    │
│  深度：50-100                                      │
│  类型：AVPacket*（PTS 已转为微秒）                  │
└────────────────────────────────────────────────────┘
```

- 单向 FIFO 队列
- 跳帧时从 Demux 端快速丢弃

### BidiRingBuffer（Decode → Render）

双向环形缓冲，支持前进/后退 peek：

```
                    write_idx (Decode 写入)
                        ↓
   ┌────┬────┬────┬────┬────┬────┬────┬────┐
   │B-2 │B-1 │ F0 │ F1 │ F2 │ F3 │    │    │
   └────┴────┴────┴────┴────┴────┴────┴────┘
              ↑
          read_idx (Render 读取)

   ← 后向缓存 (2帧) →  ← 前向缓存 (2-5帧) →
```

**特点**：
- 渲染线程 **不消费** 帧，只移动 read_idx
- 后向缓存用于逐帧回退，减少 seek
- 前向缓存深度根据解码类型调整

| 解码类型 | 前向深度 | 后向深度 | 总大小 |
|---------|---------|---------|-------|
| D3D11VA 硬解 | 2 | 2 | 4 |
| 软解 H.264 | 3 | 2 | 5 |
| 软解 H.265/AV1 | 4 | 2 | 6 |
| 软解 H.266 | 5 | 2 | 7 |

**核心操作**：
- `peek(offset)` - 查看指定偏移的帧，不移动指针
- `advance()` - 前进 1 帧（read_idx++）
- `retreat()` - 后退 1 帧（read_idx--）
- `push(frame)` - 写入新帧（write_idx++）

## 数据结构

### TextureFrame

解码输出的统一纹理封装：

| 字段 | 类型 | 说明 |
|-----|------|------|
| texture | `ID3D11Texture2D*` | RGBA 格式纹理 |
| pts_us | `int64_t` | 显示时间戳（微秒） |
| duration_us | `int64_t` | 帧持续时间（微秒） |
| is_ref | `bool` | 是否为引用（硬解） vs 拥有（软解） |

### ClockState

时钟状态：

| 字段 | 类型 | 说明 |
|-----|------|------|
| base_time_us | `int64_t` | 播放起始时的系统时间（微秒） |
| base_pts_us | `int64_t` | 起始时的视频 PTS（微秒） |
| speed | `double` | 播放速度倍率 |
| paused | `bool` | 是否暂停 |
| pause_time_us | `int64_t` | 暂停时的系统时间 |

### TrackBuffer

单个视频轨道的状态：

| 字段 | 类型 | 说明 |
|-----|------|------|
| ring | `TextureFrame[]` | 环形缓冲 |
| write_idx | `int` | Decode 写入位置 |
| read_idx | `int` | Render 读取位置 |
| last_presented_pts_us | `int64_t` | 上次上屏的 PTS（微秒） |

### RenderSink

渲染线程持有的多轨道同步状态：

| 字段 | 类型 | 说明 |
|-----|------|------|
| tracks | `TrackBuffer[]` | 各轨道缓冲 |
| clock | `ClockState` | 时钟状态 |
| master_track | `int` | 主轨道索引（驱动时钟） |

## Seek 策略

### I 帧 Seek

- **特点**：快速定位到最近的 I 帧
- **精度**：1-2 秒误差
- **场景**：拖拽进度条、快速预览
- **实现**：`av_seek_frame(..., AVSEEK_FLAG_BACKWARD)`

### 精确 Seek

- **特点**：帧级精确定位
- **流程**：
  1. Seek 到目标 PTS 之前的 I 帧
  2. 从 I 帧解码到目标 PTS
  3. 丢弃中间帧，保留目标帧
- **场景**：逐帧回退缓存未命中
- **实现**：`av_seek_frame(..., AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY)` + 解码到目标

### Seek 触发矩阵

| 场景 | Seek 类型 | 缓冲处理 |
|-----|----------|---------|
| 正常播放 | 不 seek | - |
| 逐帧前进 | 不 seek | advance() |
| 逐帧回退（缓存命中） | 不 seek | retreat() |
| 逐帧回退（缓存未命中） | 精确 seek | 清空并重新填充 |
| 拖拽进度条 | I 帧 seek | 清空并重新填充 |

## 文件拆分

```
native/video_renderer/
├── renderer.h / .cpp              # 渲染器主入口
├── clock.h / .cpp                 # 时钟管理（微秒精度）
│
├── d3d11/
│   ├── device.h / .cpp            # D3D11 设备、SwapChain
│   ├── texture.h / .cpp           # 纹理创建、格式转换
│   └── shader.h / .cpp            # 着色器编译、绑定
│
├── decode/
│   ├── demux_thread.h / .cpp      # Demux 线程，Packet 过滤
│   ├── decode_thread.h / .cpp     # Decode 线程
│   └── frame_converter.h / .cpp   # AVFrame → TextureFrame
│
├── buffer/
│   ├── packet_queue.h / .cpp      # AVPacket 队列
│   ├── bidi_ring_buffer.h / .cpp  # 双向环形缓冲
│   └── track_buffer.h / .cpp      # 单轨道缓冲
│
├── sync/
│   ├── render_sink.h / .cpp       # 多轨道同步、上屏决策
│   └── seek_controller.h / .cpp   # Seek 策略控制
│
└── shaders/
    ├── multitrack.vert            # 顶点着色器
    └── multitrack.frag            # 片段着色器
```

### 模块职责

| 模块 | 职责 |
|-----|------|
| `renderer` | 生命周期管理、外部接口 |
| `clock` | 时间管理、倍速、暂停 |
| `d3d11/device` | D3D11 设备、SwapChain |
| `d3d11/texture` | 纹理创建、格式转换 |
| `d3d11/shader` | 着色器管理 |
| `decode/demux_thread` | 文件读取、Packet 过滤、PTS 转微秒 |
| `decode/decode_thread` | 解码、格式转换 |
| `decode/frame_converter` | 统一输出 TextureFrame |
| `buffer/packet_queue` | AVPacket 线程安全队列 |
| `buffer/bidi_ring_buffer` | 双向环形缓冲 |
| `sync/render_sink` | 时钟 + 上屏决策 + PTS 对齐 |
| `sync/seek_controller` | I帧/精确 Seek |

## 着色器设计

### 多轨道 1/N 分屏显示

**顶点着色器**：全屏四边形

**片段着色器**：根据像素位置选择 track 纹理

```
┌──────────┬──────────┐
│ Track 0  │ Track 1  │    2 路视频：左右各 1/2
├──────────┼──────────┤
│ Track 2  │ Track 3  │    4 路视频：2x2 网格
└──────────┴──────────┘
```

### Uniform 变量

| Uniform | 类型 | 说明 |
|---------|------|------|
| `u_textures[N]` | `sampler2D` | 各 track 纹理 |
| `u_track_count` | `int` | 活跃 track 数量 |
| `u_canvas_aspect` | `float` | 画布宽高比 |
| `u_video_aspect[N]` | `float` | 各视频宽高比 |

## 线程安全

### 锁策略

| 资源 | 保护方式 |
|-----|---------|
| Packet Queue | mutex + condvar |
| BidiRingBuffer | mutex（读写指针分离） |
| D3D11 Immediate Context | 单线程独占 |
| ClockState | mutex 或 atomic |

### 环形缓冲并发

- Decode 线程只写 `write_idx`
- Render 线程只读/改 `read_idx`
- 两者通过 mutex 同步，但冲突窗口很小

## 性能指标

### 延迟目标

| 阶段 | 目标延迟 |
|-----|---------|
| Demux → Packet Queue | <1ms |
| 硬解 D3D11VA | 1-5ms |
| 软解 H.265 I 帧 | 50-200ms |
| 格式转换 | 1-2ms |
| 上屏决策 | <0.1ms |
| Present | vsync 间隔 |

### 内存/显存

| 资源 | 1080p RGBA |
|-----|-----------|
| 单帧纹理 | ~8MB |
| 硬解缓冲（4帧） | ~32MB |
| 软解缓冲（7帧） | ~56MB |
| Packet Queue | ~1MB |
