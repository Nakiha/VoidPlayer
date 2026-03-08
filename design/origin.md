# 软件设计文档 (SDD) - 极致性能视频对比工具

## 1. 项目概述 (Project Overview)

本项目旨在开发一款高性能的专业视频/图像画质对比工具，作为 VoidView 系统的独立播放器组件。采用 **硬件解码 → GPU 零拷贝纹理 → Shader 直接合成** 的极致性能路线，摒弃传统的 CPU 解码与像素级内存拷贝。

### 1.1 核心特性

- 双视频并排对比 (Side-by-Side) 与分屏对比 (Split-Screen)
- 硬件加速解码 + GPU 零拷贝渲染
- 精准时间戳同步
- 可被外部 PySide6 应用程序调用

### 1.2 外部集成接口

播放器组件需要支持被 VoidView 主应用程序或其他 PySide6 窗体调用：

```python
# 调用示例
from voidview_player import PlayerWindow

# 方式1: 直接传入文件路径列表
player = PlayerWindow()
player.load_sources([
    r"C:\Videos\original.mp4",
    r"C:\Videos\encoded.mp4"
])
player.show()

# 方式2: 传入网络链接
player.load_sources([
    "rtsp://192.168.1.100/stream1",
    "https://example.com/video.mp4"
])

# 方式3: 混合输入
player.load_sources([
    r"C:\Videos\reference.mkv",
    "http://cdn.example.com/test.m3u8"
])
```

**接口契约**:
- `load_sources(sources: list[str])`: 加载媒体源列表（支持本地文件/网络链接）
- `set_sync_offset(index: int, offset_ms: int)`: 设置单个视频的时间偏移
- `set_view_mode(mode: str)`: 切换视图模式 (`"side_by_side"` / `"split_screen"`)
- `play()` / `pause()` / `seek_to(timestamp_ms: int)`: 播放控制

---

## 2. 架构设计 (Architecture)

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    VoidView 主应用程序                        │
│                   (PySide6 + qfluentwidgets)                 │
└──────────────────────────┬──────────────────────────────────┘
                           │ 调用接口
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                   voidview_player (本组件)                    │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────┐ │
│  │   UI 层 (PySide6)│  │  渲染层 (OpenGL) │  │ 文件系统适配器│ │
│  │  qfluentwidgets │  │  QOpenGLWidget  │  │  (解耦模块)   │ │
│  └────────┬────────┘  └────────┬────────┘  └──────┬───────┘ │
│           │                    │                   │         │
│           └────────────────────┼───────────────────┘         │
│                                ▼                             │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                  Python 绑定层 (pybind11)                │ │
│  │              voidview_native (C++ 扩展模块)              │ │
│  └─────────────────────────┬───────────────────────────────┘ │
└────────────────────────────┼────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                  voidview_native.dll (C++ 核心)              │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │ FFmpeg 解封装 │  │ 硬件解码器   │  │ OpenGL 互操作层   │  │
│  │ (libavformat) │  │ (D3D11VA/    │  │ (EGL/WGL Context  │  │
│  │               │  │  NVDEC)      │  │  Sharing + 绑定)  │  │
│  └──────────────┘  └──────────────┘  └───────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 文件系统解耦设计

媒体源加载逻辑必须与文件系统操作解耦，通过适配器模式支持多种输入源：

```
┌─────────────────────────────────────────────────────────────┐
│                    SourceAdapter (抽象接口)                   │
├─────────────────────────────────────────────────────────────┤
│  + get_av_input_context() -> AVFormatContext*               │
│  + get_display_name() -> str                                │
│  + is_seekable() -> bool                                    │
│  + get_duration_ms() -> int                                 │
└──────────────────────────┬──────────────────────────────────┘
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
┌─────────────────┐ ┌─────────────┐ ┌───────────────┐
│ LocalFileAdapter│ │ HttpAdapter │ │ StreamAdapter │
│ (本地文件)       │ │ (HTTP/HTTPS)│ │ (RTSP/RTMP)   │
└─────────────────┘ └─────────────┘ └───────────────┘
```

**解耦要求**:
1. 播放器核心不直接处理文件路径，只通过 `SourceAdapter` 接口
2. 文件存在性检查、协议解析、缓存策略均在适配器层完成
3. 便于后续扩展：支持 S3、Azure Blob 等云存储

---

## 3. 技术栈选型 (Tech Stack)

### 3.1 Python 层

| 层级 | 技术 | 用途 |
|------|------|------|
| GUI 框架 | PySide6 | 主窗口、事件循环 |
| UI 组件 | qfluentwidgets | WinUI 3 风格控件 |
| 渲染集成 | QOpenGLWidget + PyOpenGL | GPU 渲染画布 |

### 3.2 C++ 原生层 (voidview_native)

> **重要**: 硬件帧（Hardware Frames）的句柄直接映射为 OpenGL Texture 是非常困难且容易引发段错误的操作。这部分核心逻辑必须用 C++ 实现，通过 pybind11 暴露给 Python。

| 模块 | 技术 | 职责 |
|------|------|------|
| 解封装 | FFmpeg libavformat | 读取容器格式、提取 Packet |
| 硬件解码 | FFmpeg libavcodec + D3D11VA/NVDEC | GPU 加速解码，输出硬件帧 |
| 纹理互操作 | EGL/WGL + OpenGL | 硬件帧 → OpenGL Texture 零拷贝绑定 |
| Python 绑定 | pybind11 | 暴露 C++ API 给 Python |

### 3.3 为什么需要 C++ 原生层？

直接在 Python 中通过 ctypes 处理以下操作极不稳定：

1. **FFmpeg 硬件帧生命周期管理**: `AVFrame->data[0]` 可能指向 D3D11 资源，Python GC 无法正确管理
2. **OpenGL 上下文共享**: 需要在同一线程或正确共享的上下文中操作，Python 线程模型复杂
3. **跨 API 互操作**: D3D11 → OpenGL 的纹理共享需要精确的同步原语
4. **段错误难以调试**: Python 层 crash 后无有效堆栈信息

**解决方案**: 将这些脏活累活封装在 `voidview_native.dll` 中，Python 层只需调用高层 API：

```cpp
// voidview_native 暴露的核心类 (pybind11)
class HardwareDecoder {
public:
    HardwareDecoder(const std::string& source_url);
    bool initialize(int hw_device_type);  // 0=Auto, 1=D3D11VA, 2=NVDEC
    GLuint get_texture_id();  // 返回 OpenGL 纹理 ID
    bool decode_next_frame();
    int64_t get_current_pts_ms();
    bool seek_to(int64_t timestamp_ms);
};
```

---

## 4. 核心渲染管线设计 (Core Rendering Pipeline)

### 4.1 管线流程图

```
┌──────────────┐    ┌──────────────┐    ┌───────────────────┐    ┌──────────────┐
│   解封装      │───▶│   硬件解码    │───▶│   纹理互操作       │───▶│  Shader 合成  │
│ (Demux)      │    │ (HW Decode)  │    │ (Texture Interop) │    │ (Composite)  │
└──────────────┘    └──────────────┘    └───────────────────┘    └──────────────┘
      ▲                                          │                       │
      │                                          ▼                       ▼
┌──────────────┐                        ┌──────────────┐        ┌──────────────┐
│SourceAdapter │                        │ OpenGL TexID │        │ QOpenGLWidget│
│ (文件/网络)   │                        │  (零拷贝)    │        │   (显示)      │
└──────────────┘                        └──────────────┘        └──────────────┘
```

### 4.2 各阶段详解

#### Stage 1: 解封装 (Demux)

```cpp
// C++ 层实现
AVFormatContext* fmt_ctx = avformat_alloc_context();
avformat_open_input(&fmt_ctx, source_url, nullptr, nullptr);
avformat_find_stream_info(fmt_ctx, nullptr);

// 查找视频流
int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
```

#### Stage 2: 硬件解码 (Hardware Decode)

```cpp
// 初始化硬件设备上下文
AVBufferRef* hw_device_ctx;
av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);

// 配置解码器使用硬件加速
AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

// 解码循环
AVPacket* pkt = av_packet_alloc();
AVFrame* frame = av_frame_alloc();
while (av_read_frame(fmt_ctx, pkt) >= 0) {
    avcodec_send_packet(codec_ctx, pkt);
    avcodec_receive_frame(codec_ctx, frame);
    // frame->format == AV_PIX_FMT_D3D11 (硬件帧!)
}
```

#### Stage 3: 纹理互操作 (Texture Interop) - **核心难点**

> 这是最容易出错的部分，必须在 C++ 中精心处理。

```cpp
// D3D11 纹理 → OpenGL 纹理的互操作流程
// 1. 从 AVFrame 提取 D3D11 纹理句柄
ID3D11Texture2D* d3d11_tex = (ID3D11Texture2D*)frame->data[0];
int subresource_index = (intptr_t)frame->data[1];

// 2. 获取 DXGI 共享句柄
HANDLE shared_handle;
d3d11_tex->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgi_resource);
dxgi_resource->GetSharedHandle(&shared_handle);

// 3. 在 OpenGL 上下文中打开共享资源
// 需要使用 GL_NV_DX_interop 或 WGL_NV_DX_interop 扩展
glDXSetResourceShareHandleNV(d3d11_tex, shared_handle);
GLuint gl_tex_id;
glGenTextures(1, &gl_tex_id);
glDXObjectRegisterNV(gl_tex_id, d3d11_tex, GL_TEXTURE_2D);

// 4. 返回 OpenGL 纹理 ID 给 Python 层
return gl_tex_id;
```

#### Stage 4: Shader 合成 (Shader Composition)

```glsl
// fragment_shader.glsl
#version 330 core

uniform sampler2D texA;      // 视频 A 纹理
uniform sampler2D texB;      // 视频 B 纹理
uniform float split_position; // 分割线位置 (0.0 ~ 1.0)
uniform mat4 transform;       // 缩放+平移矩阵
uniform int view_mode;        // 0: 并排, 1: 分屏

in vec2 uv;
out vec4 frag_color;

void main() {
    vec2 transformed_uv = (transform * vec4(uv, 0.0, 1.0)).xy;

    if (view_mode == 0) {
        // 并排模式: 左半边显示 texA, 右半边显示 texB
        vec4 color_a = texture(texA, transformed_uv);
        vec4 color_b = texture(texB, transformed_uv);
        frag_color = (uv.x < 0.5) ? color_a : color_b;

        // 中线分割
        if (abs(uv.x - 0.5) < 0.002) {
            frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        }
    } else {
        // 分屏模式: 根据 split_position 动态分割
        vec4 color_a = texture(texA, transformed_uv);
        vec4 color_b = texture(texB, transformed_uv);
        frag_color = (uv.x < split_position) ? color_a : color_b;
    }
}
```

---

## 5. 界面布局与模块划分 (Fluent UI Modules)

### 5.1 主窗口结构

```
┌──────────────────────────────────────────────────────────────────┐
│                         FluentWindow                              │
├────────────────┬─────────────────────────────────────────────────┤
│                │                                                  │
│  NavigationBar │              QOpenGLWidget                       │
│                │           (全屏渲染画布)                          │
│  ┌───────────┐ │                                                  │
│  │ Home      │ │      ┌─────────────┬─────────────┐              │
│  │ Side-by-  │ │      │   视频 A    │   视频 B    │              │
│  │ Side      │ │      │             │             │              │
│  │ Split     │ │      │             │             │              │
│  │ Settings  │ │      └─────────────┴─────────────┘              │
│  └───────────┘ │                                                  │
│                ├─────────────────────────────────────────────────┤
│                │         Control Panel (亚克力半透明)              │
│                │  [▶] [⏸] [⏮] [⏭]  ═════════════════════  📊    │
│                │         Timeline Slider                          │
│                └─────────────────────────────────────────────────┘
└────────────────┴─────────────────────────────────────────────────┘
```

### 5.2 控制面板组件

| 组件 | qfluentwidgets 控件 | 功能 |
|------|---------------------|------|
| 播放/暂停 | TogglePushButton | 切换播放状态 |
| 上一帧/下一帧 | TransparentToolButton | 逐帧步进 |
| 时间轴 | Slider | 全局进度控制 |
| 源列表 | CardWidget 列表 | 显示已加载的视频源 |
| 视图模式 | SegmentedWidget | 切换并排/分屏 |

---

## 6. 关键实现难点与解决方案

### 6.1 OpenGL Context 与多线程

**问题**: FFmpeg 解码在后台线程，OpenGL 渲染在主线程，纹理需要在两个线程间共享。

**解决方案**:
```python
# Python 层
class GLWidget(QOpenGLWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.decoder_a = HardwareDecoder()  # C++ 对象
        self.decoder_b = HardwareDecoder()

    def initializeGL(self):
        # 在 OpenGL 上下文初始化后，设置解码器
        self.decoder_a.initialize_with_context(self.context())
        self.decoder_b.initialize_with_context(self.context())
```

```cpp
// C++ 层 - 确保上下文共享
void HardwareDecoder::initialize_with_context(void* gl_context) {
    QOpenGLContext* ctx = static_cast<QOpenGLContext*>(gl_context);
    // 创建共享的解码上下文
    decode_context_ = new QOpenGLContext();
    decode_context_->setShareContext(ctx);
    decode_context_->create();
}
```

### 6.2 精准时间戳同步 (PTS Synchronization)

```python
class SyncController:
    def __init__(self):
        self.master_clock = 0  # 虚拟主时钟 (毫秒)
        self.offsets: dict[int, int] = {}  # 每个视频的时间偏移

    def get_frame_for_source(self, source_id: int, decoder: HardwareDecoder) -> GLuint:
        """根据主时钟和偏移量，返回对应的纹理"""
        target_pts = self.master_clock + self.offsets.get(source_id, 0)
        decoder.seek_to(target_pts)
        decoder.decode_next_frame()
        return decoder.get_texture_id()
```

### 6.3 避免段错误的最佳实践

1. **所有 FFmpeg 调用都在 C++ 层**: 不要通过 ctypes 在 Python 中直接操作 AVFrame
2. **纹理生命周期由 C++ 管理**: Python 只持有 GLuint (整数)，不管理内存
3. **使用 RAII 包装资源**: C++ 层用 `std::unique_ptr` 管理所有 FFmpeg 资源
4. **添加调试检查**: 在 Debug 模式下验证所有 OpenGL 调用

---

## 7. 目录结构 (Project Structure)

```
VoidPlayer/
├── libs/
│   └── ffmpeg/                       # FFmpeg 预编译库 (Windows x64)
│       ├── bin/                      # DLL 文件 (运行时需要)
│       │   ├── avcodec-62.dll
│       │   ├── avformat-62.dll
│       │   ├── avutil-60.dll
│       │   ├── swresample-6.dll
│       │   └── swscale-9.dll
│       ├── include/                  # C 头文件 (编译时需要)
│       │   └── libavcodec/
│       │   └── libavformat/
│       │   └── libavutil/
│       │   └── ...
│       └── lib/                      # 导入库 (.lib)
│           ├── avcodec.lib
│           ├── avformat.lib
│           └── ...
├── player/                  # 播放器 Python 模块
│   ├── __init__.py
│   ├── player_window.py              # 主窗口 (FluentWindow)
│   ├── gl_widget.py                  # QOpenGLWidget 实现
│   ├── sync_controller.py            # 时间同步控制器
│   ├── adapters/
│   │   ├── __init__.py
│   │   ├── base.py                   # SourceAdapter 抽象基类
│   │   ├── local_file.py             # 本地文件适配器
│   │   ├── http_source.py            # HTTP/HTTPS 适配器
│   │   └── stream.py                 # RTSP/RTMP 适配器
│   └── shaders/
│       ├── vertex.glsl               # 顶点着色器
│       └── fragment.glsl             # 片段着色器
│
└── native/                  # C++ 扩展模块 (单独构建)
    ├── CMakeLists.txt
    ├── src/
    │   ├── bindings.cpp              # pybind11 绑定
    │   ├── hardware_decoder.cpp      # 硬件解码器封装
    │   ├── texture_interop.cpp       # D3D11→OpenGL 互操作
    │   └── frame_queue.cpp           # 帧队列 (线程安全)
    ├── include/
    │   └── voidview_native/
    │       ├── hardware_decoder.hpp
    │       └── texture_interop.hpp
    └── build.py                      # 构建脚本
```

> **FFmpeg 配置说明**:
> - CMake 构建时需要设置 `FFMPEG_ROOT` 指向 `libs/ffmpeg`
> - 运行时需要将 `libs/ffmpeg/bin` 添加到 PATH 或复制 DLL 到输出目录

---

## 8. 分阶段实施计划 (Phased Implementation Plan)

> **重要**: 这是一个涉及底层 C++ 和 GPU Shader 的硬核项目，务必按阶段实施。

### Phase 1: C++ 基础设施搭建 (优先级最高)

**目标**: 建立可工作的 `voidview_native.dll`

- [ ] 配置 CMake + pybind11 项目结构
- [ ] 实现基础的 FFmpeg 解封装 (`avformat_open_input`)
- [ ] 实现硬件解码器初始化 (D3D11VA/NVDEC)
- [ ] 实现单帧解码 → 硬件帧提取
- [ ] 编写 Python 测试用例验证解码功能

### Phase 2: 纹理互操作 (核心难点)

**目标**: 实现硬件帧 → OpenGL 纹理的零拷贝绑定

- [ ] 实现 D3D11 纹理句柄提取
- [ ] 实现 WGL_NV_DX_interop 扩展绑定
- [ ] 验证纹理在 QOpenGLWidget 中正确显示
- [ ] 处理资源同步与释放

### Phase 3: Python 层 UI 框架

**目标**: 搭建可用的 UI 骨架

- [ ] 实现 PlayerWindow (FluentWindow 子类)
- [ ] 实现 GLWidget (QOpenGLWidget 子类)
- [ ] 实现 SourceAdapter 抽象层
- [ ] 实现本地文件和 HTTP 适配器
- [ ] 实现基本的并排/分屏切换

### Phase 4: 多视频同步播放

**目标**: 完整的播放控制功能

- [ ] 实现 SyncController 全局时钟
- [ ] 实现播放/暂停/Seek
- [ ] 实现时间偏移调整
- [ ] 实现逐帧步进

### Phase 5: 集成与优化

**目标**: 与 VoidView 主应用集成

- [ ] 实现外部调用接口 (`load_sources()` 等)
- [ ] 性能分析与优化
- [ ] 内存泄漏检查
- [ ] 错误处理与日志完善

---

## 9. 风险与缓解措施

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|----------|
| D3D11→OpenGL 互操作在某些 GPU 上失败 | 中 | 高 | 实现 CPU 回退路径作为 Plan B |
| pybind11 编译问题 | 低 | 中 | 提供预编译的 wheel 包 |
| 多线程同步导致死锁 | 中 | 高 | 使用成熟的线程安全队列 |
| 网络流延迟处理 | 中 | 中 | 实现自适应缓冲策略 |

---

## 10. 参考资源

- [FFmpeg Hardware Acceleration](https://trac.ffmpeg.org/wiki/HWAccelIntro)
- [pybind11 Documentation](https://pybind11.readthedocs.io/)
- [WGL_NV_DX_interop Extension](https://www.khronos.org/registry/OpenGL/extensions/NV/WGL_NV_DX_interop.txt)
- [QOpenGLWidget Documentation](https://doc.qt.io/qt-6/qopenglwidget.html)
