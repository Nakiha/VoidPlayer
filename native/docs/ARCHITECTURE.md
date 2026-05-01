# Native 模块架构概览

> 本文档是 native 模块的入口文档，各子系统的详细设计请参阅底部链接。

## 模块定位

C++ DLL 视频渲染引擎，基于 FFmpeg demux + D3D11VA 硬解/软解，为 VoidPlayer 提供：

- 多路视频同时解码（1-4 轨道）
- 帧级 PTS 对齐同步上屏
- 逐帧前进/后退
- I 帧 Seek / 精确 Seek
- 倍速播放（保持时钟连续性）
- Headless 三缓冲 shared texture，上屏到 Flutter Texture
- 自动化截图/hash 验证入口（通过 Flutter action 调用 native front buffer capture）

## 目录结构

```
native/
├── CMakeLists.txt                  # 主构建配置
├── build.py                        # Python 构建脚本
├── probe_hw.cpp                    # 硬件能力探测工具
├── common/
│   └── logging.h/cpp               # spdlog 配置 + 崩溃处理
├── media/                          # 容器读取 / stream 分发（renderer 与 audio 共享）
│   ├── demux_thread.h/cpp          # FFmpeg demux + packet route
│   ├── packet_queue.h/cpp          # AVPacket 线程安全队列
│   └── seek_controller.h/cpp       # Seek 请求协调
├── audio/
│   └── audio_engine.h/cpp          # 音频解码 + WinMM 输出（单 audible track）
├── player/
│   └── native_player.h/cpp         # native 播放 facade，平级拥有 playback + renderer
├── playback/
│   └── playback_controller.h/cpp   # 播放级控制，拥有 Clock + AudioEngine
├── video_renderer/                 # 核心静态库
│   ├── renderer.h/cpp              # 渲染器主入口
│   ├── clock.h/cpp                 # PTS 时钟（可注入时间源）
│   ├── exports/                    # renderer 的 FFI / pybind11 导出
│   ├── demo/                       # renderer Python 交互式 Demo
│   ├── benchmarks/                 # renderer 管线性能基准
│   ├── d3d11/                      # D3D11 后端
│   │   ├── device.h/cpp            # 设备 / SwapChain
│   │   ├── texture.h/cpp           # 纹理创建、上传、池化、shared texture 打开
│   │   ├── frame_presenter.h/cpp   # TextureFrame -> D3D11 SRV 准备
│   │   ├── headless_output.h/cpp   # Flutter shared texture 三缓冲输出
│   │   └── shader.h/cpp            # HLSL 编译管理
│   ├── decode/                     # 解码管线
│   │   ├── decode_thread.h/cpp     # Decode 线程
│   │   ├── frame_converter.h/cpp   # AVFrame → TextureFrame
│   │   └── hw/                     # 硬件解码 Provider
│   │       ├── hw_decode_provider.h/cpp
│   │       └── d3d11va_provider.h/cpp
│   ├── buffer/                     # 帧缓冲
│   │   ├── bidi_ring_buffer.h/cpp  # 双向环形缓冲
│   │   └── track_buffer.h/cpp      # 轨道状态 + Preroll
│   ├── sync/                       # 同步
│   │   └── render_sink.h/cpp       # 上屏决策
│   └── shaders/
│       └── multitrack.hlsl         # RGBA + NV12 着色器
├── tests/                          # Catch2 单元测试
│   ├── renderer/
│   ├── analysis/
│   └── ffi/
└── docs/
```

## 类层级

```
Renderer                          # 主入口，生命周期管理
├── Clock                         # 可注入时间源，PTS 时钟
├── D3D11Device                   # GPU 设备 + 可选窗口 SwapChain
├── ShaderManager                 # HLSL 编译
├── D3D11FramePresenter           # 每轨 frame 的 SRV/上传/NV12 copy 缓存
├── D3D11HeadlessOutput           # Flutter Texture shared handle 三缓冲
├── RenderSink                    # 上屏决策，PTS 对齐
│
└── TrackPipeline[N]              # 每路视频一个
    ├── PacketQueue               # AVPacket 有界阻塞队列
    ├── TrackBuffer               # 状态机 + Preroll
    │   └── BidiRingBuffer        # 双向环形缓冲
    ├── DemuxThread               # media 层文件读取 / packet 分发线程
    ├── DecodeThread              # 解码线程
    │   └── FrameConverter        # YUV→RGBA / NV12 包装
    │   └── HwDecodeProvider?     # 硬解（可选）
    └── SeekController            # Seek 请求协调
```

## 数据流总览

```
Video File
  → [DemuxThread] → AVPacket (stream time_base)
    → [PacketQueue] →
      → [DecodeThread] → AVFrame (YUV/D3D11VA)
        → [FrameConverter] → TextureFrame
          → [TrackBuffer/BidiRingBuffer] →
            → [RenderSink] → PresentDecision
              → [D3D11FramePresenter] → [D3D11 Draw]
                → SwapChain 或 D3D11HeadlessOutput shared texture
```

## 当前硬解路径

| 路径 | 典型 codec | 说明 |
|------|------------|------|
| D3D11VA renderer-owned NV12 | H.264/H.265 | decoder surface copy 到 renderer-owned NV12 texture，再由 shader 采样 |
| D3D11VA hwdownload | AV1/VP9 | 硬解后 `av_hwframe_transfer_data` 到 CPU RGBA，再走上传路径 |
| 软件 fallback | 硬解不可用/打开失败 | AV1 软件 fallback 优先 `libdav1d` |

## 详细文档索引

| 文档 | 内容 |
|------|------|
| [线程模型](THREADING_MODEL.md) | 线程角色、锁策略、渲染循环 |
| [数据管线](DATA_PIPELINE.md) | 帧格式变迁、零拷贝路径 |
| [时钟与同步](CLOCK_AND_SYNC.md) | Clock API、倍速、A/V 同步算法 |
| [缓冲设计](BUFFER_DESIGN.md) | 队列、环形缓冲、状态机、Preroll |
| [解码管线](DECODE_PIPELINE.md) | 软解/硬解路径、HwDecodeProvider |
| [Seek 策略](SEEK_STRATEGY.md) | SeekController、触发矩阵 |
| [D3D11 后端](D3D11_BACKEND.md) | 设备、纹理、着色器、NV12 零拷贝 |
| [FFI 与绑定](FFI_AND_BINDINGS.md) | C FFI API、Python 绑定 |
| [构建与测试](BUILD_AND_TEST.md) | CMake 目标、测试、基准、Demo |
