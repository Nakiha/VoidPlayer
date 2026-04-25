# FFI 与绑定

## C FFI

头文件: `video_renderer/exports/ffi_exports.h`

所有导出函数使用 `naki_vr_` 前缀，通过宏 `NAKI_VR_FFI_EXPORT` 控制导出。

### 句柄类型

```c
typedef void* naki_vr_renderer_t;  // 不透明句柄
```

### 配置结构

```c
typedef struct naki_vr_log_config_t {
    const char* pattern;        // 日志格式，默认 "[%Y-%m-%d %H:%M:%S.%e] [%l] %v"
    const char* file_path;      // 日志文件路径，空 = 无文件日志
    size_t max_file_size;       // 单文件大小上限，默认 5MB
    int max_files;              // 轮转文件数，默认 3
    int level;                  // spdlog 级别: 0=trace..6=off
} naki_vr_log_config_t;

typedef struct naki_vr_renderer_config_t {
    const char** video_paths;   // NULL 终止的文件路径数组
    int64_t hwnd;               // 窗口句柄
    int width, height;          // 初始尺寸
    int use_hardware_decode;    // 0=软解, 1=硬解
    naki_vr_log_config_t log_config;
} naki_vr_renderer_config_t;
```

### API 分类

| 分类 | 函数 |
|------|------|
| 生命周期 | create / destroy / initialize / shutdown |
| 播放控制 | play / pause / resume / seek / seek_typed / set_speed |
| 逐帧 | step_forward / step_backward |
| 查询 | is_playing / is_initialized / current_pts_us / current_speed / track_count / duration_us |
| 日志 | configure_logging / install_crash_handler / remove_crash_handler |

### Seek 类型常量

```c
#define NAKI_VR_SEEK_KEYFRAME  0
#define NAKI_VR_SEEK_EXACT     1
```

---

## Python 绑定

文件: `video_renderer/exports/bindings.cpp`

使用 pybind11 绑定，导出类：

| Python 类 | C++ 对应 |
|-----------|---------|
| `Renderer` | `vr::Renderer` |
| `RendererConfig` | `vr::RendererConfig` |
| `LogConfig` | `vr::LogConfig` |
| `SeekType` 枚举 | `Keyframe` / `Exact` |

### 独立函数

```python
configure_logging(LogConfig)
install_crash_handler(str)   # crash_dir
```

---

## 构建输出

| 目标 | 输出路径 | 说明 |
|------|---------|------|
| video_renderer_ffi | dist/ffi/ | DLL + 头文件 |
| video_renderer_native | dist/python/ | .pyd + FFmpeg DLLs |

C FFI 消费者需链接 `video_renderer_ffi.dll` 并包含 `ffi_exports.h`。
Python 消费者 `import video_renderer_native` 即可。
