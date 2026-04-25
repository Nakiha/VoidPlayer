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
├── video_renderer/                 # 核心静态库
│   ├── renderer.h/cpp              # 渲染器主入口
│   ├── clock.h/cpp                 # PTS 时钟（可注入时间源）
│   ├── common/logging.h/cpp        # spdlog 配置 + 崩溃处理
│   ├── d3d11/                      # D3D11 后端
│   │   ├── device.h/cpp            # 设备 / SwapChain / Headless render target
│   │   ├── texture.h/cpp           # 纹理创建、上传、池化
│   │   └── shader.h/cpp            # HLSL 编译管理
│   ├── decode/                     # 解码管线
│   │   ├── demux_thread.h/cpp      # Demux 线程
│   │   ├── decode_thread.h/cpp     # Decode 线程
│   │   ├── frame_converter.h/cpp   # AVFrame → TextureFrame
│   │   └── hw/                     # 硬件解码 Provider
│   │       ├── hw_decode_provider.h/cpp
│   │       └── d3d11va_provider.h/cpp
│   ├── buffer/                     # 帧缓冲
│   │   ├── packet_queue.h/cpp      # AVPacket 线程安全队列
│   │   ├── bidi_ring_buffer.h/cpp  # 双向环形缓冲
│   │   └── track_buffer.h/cpp      # 轨道状态 + Preroll
│   ├── sync/                       # 同步
│   │   ├── render_sink.h/cpp       # 上屏决策
│   │   └── seek_controller.h/cpp   # Seek 协调
│   └── shaders/
│       └── multitrack.hlsl         # RGBA + NV12 着色器
├── exports/                        # FFI 层
│   ├── ffi_exports.h/cpp           # C FFI (naki_vr_*)
│   ├── bindings.cpp                # pybind11 绑定
│   └── __init__.py
├── tests/                          # Catch2 单元测试
├── benchmarks/                     # 管线性能基准
└── demo/                           # Python 交互式 Demo
```

## 类层级

```
Renderer                          # 主入口，生命周期管理
├── Clock                         # 可注入时间源，PTS 时钟
├── D3D11Device                   # GPU 设备 + SwapChain
├── ShaderManager                 # HLSL 编译
├── RenderSink                    # 上屏决策，PTS 对齐
│
└── TrackPipeline[N]              # 每路视频一个
    ├── PacketQueue               # AVPacket 有界阻塞队列
    ├── TrackBuffer               # 状态机 + Preroll
    │   └── BidiRingBuffer        # 双向环形缓冲
    ├── DemuxThread               # 文件读取线程
    ├── DecodeThread              # 解码线程
    │   └── FrameConverter        # YUV→RGBA / NV12 包装
    │   └── HwDecodeProvider?     # 硬解（可选）
    └── SeekController            # Seek 请求协调
```

## 数据流总览

```
Video File
  → [DemuxThread] → AVPacket (PTS=μs)
    → [PacketQueue] →
      → [DecodeThread] → AVFrame (YUV/D3D11VA)
        → [FrameConverter] → TextureFrame
          → [TrackBuffer/BidiRingBuffer] →
            → [RenderSink] → PresentDecision
              → [D3D11 Draw] → SwapChain 或 Headless Shared Texture
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
