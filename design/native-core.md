# Native Core - C++ 原生层设计文档

> **版本**: 1.1
> **状态**: 已实现
> **依赖**: FFmpeg 6.x, pybind11, OpenGL 3.3+

---

## 1. 模块概述

### 1.1 职责定义

`voidview_native.dll` 负责性能敏感的底层操作：

| 职责 | 说明 |
|------|------|
| 媒体解封装 | FFmpeg libavformat 读取容器格式 |
| 硬件解码 | D3D11VA/NVDEC GPU 加速解码 |
| 纹理互操作 | D3D11 → OpenGL 零拷贝绑定 |
| Python 绑定 | pybind11 暴露高层 API |

### 1.2 为什么需要 C++ 原生层

| 风险 | Python 直接处理 | C++ 封装后 |
|------|----------------|-----------|
| 硬件帧生命周期 | GC 无法管理 D3D11 资源 | RAII 自动管理 |
| OpenGL 上下文共享 | 线程模型复杂 | 明确的上下文传递 |
| 跨 API 互操作 | 极易段错误 | 封装同步原语 |
| 调试困难 | crash 无堆栈 | 完整调试信息 |

---

## 2. 架构设计

### 2.1 模块结构

```
voidview_native.dll
├── HardwareDecoder      # 主入口类，Python 直接调用
├── TextureInterop       # D3D11↔OpenGL 互操作
├── FrameQueue           # 线程安全帧队列
└── bindings             # pybind11 绑定层
```

### 2.2 渲染管线

```
┌──────────────┐    ┌──────────────┐    ┌───────────────┐    ┌──────────────┐
│   解封装      │───▶│   硬件解码    │───▶│   纹理互操作   │───▶│  Shader 合成  │
│ (Demux)      │    │ (HW Decode)  │    │ (Interop)     │    │ (Composite)  │
└──────────────┘    └──────────────┘    └───────────────┘    └──────────────┘
      │                                        │
      ▼                                        ▼
 SourceAdapter                            GLuint tex_id
  (URL 字符串)                             (Python 可用)
```

### 2.3 纹理互操作流程 (零拷贝实现)

```
FFmpeg D3D11VA 解码
        │
        ▼
AVFrame (NV12 D3D11 硬件帧)
        │
        ▼ ID3D11VideoProcessor
GPU NV12 → RGBA 转换 (零拷贝)
        │
        ▼ ID3D11Texture2D (RGBA)
        │
        ▼ WGL_NV_DX_interop
glDXSetResourceShareHandleNV()
glDXRegisterObjectNV()
        │
        ▼
GLuint texture_id ──────────────▶ OpenGL 渲染
```

**关键点:**
1. D3D11VA 解码输出 NV12 格式纹理
2. VideoProcessor 在 GPU 上完成 NV12→RGBA 转换（无 CPU 拷贝）
3. WGL_NV_DX_interop 将 D3D11 RGBA 纹理共享给 OpenGL
4. 整个流程完全在 GPU 上完成（零拷贝）

---

## 3. 接口规范

### 3.1 HardwareDecoder 类

```cpp
class HardwareDecoder {
public:
    // 构造与初始化
    HardwareDecoder(const std::string& source_url);
    bool initialize(int hw_device_type = 0);  // 0=Auto, 1=D3D11VA, 2=NVDEC
    void set_opengl_context(void* gl_context);

    // 解码操作
    bool decode_next_frame();
    bool seek_to(int64_t timestamp_ms);

    // 状态查询
    GLuint get_texture_id();        // OpenGL 纹理 ID
    int64_t get_current_pts_ms();   // 当前帧时间戳
    int64_t get_duration_ms();      // 总时长
    bool is_seekable();
    bool is_eof();
    bool has_error();
    std::string get_error_message();
};
```

### 3.2 Python 调用示例

```python
import voidview_native

decoder = voidview_native.HardwareDecoder("video.mp4")
decoder.initialize(0)  # Auto detect

while decoder.decode_next_frame():
    tex_id = decoder.get_texture_id()
    pts = decoder.get_current_pts_ms()
    # 使用 tex_id 进行 OpenGL 渲染
```

---

## 4. 数据结构

### 4.1 FrameQueue

| 字段 | 类型 | 说明 |
|------|------|------|
| queue_ | std::queue<AVFrame*> | 帧队列 |
| mutex_ | std::mutex | 线程安全锁 |
| max_size_ | size_t | 最大容量 (默认 4) |

### 4.2 解码状态

| 状态 | 触发条件 |
|------|---------|
| UNINITIALIZED | 构造后 |
| READY | initialize() 成功 |
| DECODING | decode_next_frame() 中 |
| EOF | av_read_frame 返回 EOF |
| ERROR | 任何错误 |

---

## 5. 错误处理策略

| 错误类型 | 处理方式 |
|---------|---------|
| 文件打开失败 | 返回 false，设置 error_message |
| 解码器初始化失败 | 尝试软件解码回退 |
| 纹理绑定失败 | 返回 false，释放资源 |
| EOF | 设置 is_eof = true |

---

## 6. 构建配置

### 6.1 依赖

| 库 | 版本 | 用途 |
|---|------|------|
| FFmpeg | 6.x | 解封装 + 解码 |
| pybind11 | 2.11+ | Python 绑定 |
| OpenGL | 3.3+ | 纹理渲染 |

### 6.2 CMake 变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| FFMPEG_ROOT | ../libs/ffmpeg | FFmpeg 安装路径 |
| CMAKE_BUILD_TYPE | Release | 构建类型 |

### 6.3 输出

- Windows: `voidview_native.pyd`
- 位置: `player/voidview_native.pyd`

---

## 7. 风险与缓解

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|----------|
| WGL_NV_DX_interop 不可用 | 低 | 高 | CPU 回退路径（已实现） |
| 显卡驱动兼容性 | 中 | 高 | 支持 NVDEC 备选 |
| VideoProcessor 不支持 | 低 | 中 | CPU YUV→RGB 回退（已实现） |
| 内存泄漏 | 低 | 高 | RAII 智能指针管理 |

**已实现的回退机制:**
- GPU VideoProcessor 失败 → CPU YUV→RGB 转换
- WGL_NV_DX_interop 不可用 → 错误报告

---

## 8. 验收标准

- [x] 单视频解码成功，纹理 ID 有效
- [x] D3D11VA 硬件解码正常工作
- [x] D3D11 VideoProcessor GPU NV12→RGBA 转换（零拷贝）
- [x] WGL_NV_DX_interop 纹理共享
- [x] Python 可正常 import

## 9. 实现状态

**已完成模块:**
- `HardwareDecoder` - FFmpeg D3D11VA 硬件解码
- `TextureInterop` - D3D11 VideoProcessor + WGL_NV_DX_interop 零拷贝纹理共享
- `FrameQueue` - 帧队列管理
- pybind11 Python 绑定

**文件结构:**
```
native/
├── CMakeLists.txt
├── build.py
├── include/voidview_native/
│   ├── hardware_decoder.hpp
│   ├── texture_interop.hpp
│   └── frame_queue.hpp
└── src/
    ├── hardware_decoder.cpp
    ├── texture_interop.cpp
    ├── frame_queue.cpp
    └── bindings.cpp
```

**测试:**
- `tests/test_opengl_demo.py` - OpenGL 渲染演示

---

## 9. 依赖关系

```
┌─────────────────┐
│  native-core.md │  ◀── 本文档
└────────┬────────┘
         │ 提供解码器
         ▼
┌─────────────────┐
│source-adapter.md│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  ui-layer.md    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ integration.md  │
└─────────────────┘
```

**下一步**: [source-adapter.md](source-adapter.md)
