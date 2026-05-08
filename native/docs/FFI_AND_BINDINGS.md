# FFI 与绑定

## C FFI

头文件: `video_renderer/exports/ffi_exports.h`

所有导出函数使用 `naki_vr_` 前缀，通过宏 `NAKI_VR_FFI_EXPORT` 控制导出。

### 句柄类型

```c
typedef void* naki_vr_player_t;  // 不透明句柄
```

### 配置结构

```c
#define NAKI_VR_ABI_VERSION 1u

typedef struct naki_vr_log_config_t {
    uint32_t size;          // sizeof(naki_vr_log_config_t)
    uint32_t abi_version;   // NAKI_VR_ABI_VERSION
    const char* pattern;        // 日志格式，默认 "[%Y-%m-%d %H:%M:%S.%e] [%l] %v"
    const char* file_path;      // 日志文件路径，空 = 无文件日志
    size_t max_file_size;       // 单文件大小上限，默认 5MB
    int max_files;              // 轮转文件数，默认 3
    int level;                  // NAKI_VR_LOG_*: 0=trace..6=off
} naki_vr_log_config_t;

typedef struct naki_vr_player_config_t {
    uint32_t size;              // sizeof(naki_vr_player_config_t)
    uint32_t abi_version;       // NAKI_VR_ABI_VERSION
    const char** video_paths;   // NULL 终止的文件路径数组
    int64_t hwnd;               // 窗口句柄
    int width, height;          // 初始尺寸
    int use_hardware_decode;    // 0=软解, 1=硬解
    naki_vr_log_config_t log_config;
} naki_vr_player_config_t;
```

所有跨 FFI 的 config/layout struct 必须先填 `size` 和 `abi_version`。FFI 层会校验 struct 大小、ABI 版本、log level、seek type、layout mode 和 pixel-size mode；失败时原有 bool/int API 仍返回失败值，详细原因通过 `naki_vr_last_error()` 查询。

### API 分类

| 分类 | 函数 |
|------|------|
| ABI / 错误 | abi_version / last_error |
| 生命周期 | create / destroy / initialize / shutdown |
| 播放控制 | play / pause / resume / seek / seek_typed / set_speed |
| 逐帧 | step_forward / step_backward |
| 查询 | is_playing / is_initialized / current_pts_us / current_speed / track_count / duration_us |
| 日志 | configure_logging / install_crash_handler / remove_crash_handler |

### Status / last error

```c
typedef enum naki_vr_status_t {
    NAKI_VR_OK = 0,
    NAKI_VR_ERR_INVALID_ARGUMENT = 1,
    NAKI_VR_ERR_NOT_INITIALIZED = 2,
    NAKI_VR_ERR_OPEN_FAILED = 3,
    NAKI_VR_ERR_INTERNAL = 1000,
} naki_vr_status_t;
```

`naki_vr_last_error(player, buf, cap)` 返回最近一次 FFI 调用的 status，并在 `buf` 非空时复制一段诊断文本。当前实现使用线程本地 last-error 状态，`player` 参数保留给后续 per-player 错误状态。

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
| `NativePlayer` | `vr::NativePlayer` |
| `NativePlayerConfig` | `vr::NativePlayerConfig` |
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
| video_renderer_ffi | `native/build-msvc/dist/ffi/` | DLL + 头文件 |
| video_renderer_native | `native/build-msvc/dist/python/` | .pyd + FFmpeg DLLs |

C FFI 消费者需链接 `video_renderer_ffi.dll` 并包含 `ffi_exports.h`。
Python 消费者 `import video_renderer_native` 即可。
