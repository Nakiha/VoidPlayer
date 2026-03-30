# 构建与测试

## 构建系统

### CMake 目标

| 目标 | 类型 | 说明 |
|------|------|------|
| video_renderer_lib | STATIC | 核心管线库 |
| video_renderer_ffi | SHARED | C FFI DLL（naki_vr_*） |
| video_renderer_native | MODULE | Python 扩展 (.pyd) |
| video_renderer_tests | EXE | Catch2 单元测试 |
| test_ffi_c | EXE | C FFI 验证 |
| probe_hw | EXE | 硬件能力探测 |
| pipeline_bench | EXE | 性能基准 |

### 依赖 (FetchContent)

| 依赖 | 版本 | 用途 |
|------|------|------|
| spdlog | v1.15.2 | 日志 |
| Catch2 | v3.8.1 | 测试框架 |
| pybind11 | find_package | Python 绑定 |
| FFmpeg | 外部 `../libs/ffmpeg` | 解码 |

### 构建脚本

```bash
python native/build.py                 # 完整构建 + 测试
python native/build.py --build-only    # 仅构建
python native/build.py --test-only     # 仅测试
python native/build.py --benchmarks-only  # 仅基准
python native/build.py --debug         # Debug 构建
```

---

## 测试

框架: Catch2 v3 (`Catch2WithMain`)

### 测试文件

| 文件 | 测试目标 |
|------|---------|
| test_clock | 时钟精度、暂停恢复、倍速 |
| test_packet_queue | 有界队列、abort、EOF |
| test_bidi_ring_buffer | 双向 peek/advance/retreat |
| test_track_buffer | 状态转换、Preroll |
| test_demux_thread | 启停、DemuxStats |
| test_decode_thread | 初始化、硬解/软解 |
| test_frame_converter | YUV→RGBA、NV12 |
| test_d3d11_device | 设备创建、SwapChain |
| test_d3d11_texture | 纹理创建、上传 |
| test_d3d11_shader | HLSL 编译 |
| test_render_sink | 上屏决策逻辑 |
| test_renderer_integration | 端到端播放 |
| test_ffi_c | C FFI ABI 验证 |

### 测试视频

环境变量 `VIDEO_TEST_DIR` 指向 `../resources/video`。

---

## 性能基准

可执行文件: `pipeline_bench.exe`

### 阶段

| 基准 | 测量内容 |
|------|---------|
| bench_demux_only | 解复帧率 |
| bench_demux_decode | 解复+软解 |
| bench_demux_decode_sws | + sws_scale YUV→RGBA |
| bench_demux_decode_sws_d3d11 | + D3D11 纹理上传（新建） |
| bench_demux_decode_sws_d3d11_reuse | + D3D11 纹理上传（池化） |
| bench_full_pipeline | 完整管线含 Present |

输出: FPS、ms/frame、瓶颈定位。

---

## Demo

### demo_video_renderer.py

交互式 PySide6 窗口，键盘控制：

| 按键 | 功能 |
|------|------|
| Space | 播放/暂停 |
| ←/→ | 逐帧后退/前进 |
| Shift+←/→ | ±1s Seek |
| 标题栏 | 实时 PTS 显示 |

### demo_seek.py

自动化演示：播放 3s → seek 回退 → 逐帧后退 → 逐帧前进 → 播放 2s → seek 前进。
