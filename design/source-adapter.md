# Source Adapter - 文件系统适配器设计文档

> **版本**: 1.0
> **状态**: 设计阶段
> **依赖**: Python 3.10+, 无外部依赖

---

## 1. 模块概述

### 1.1 设计目标

Source Adapter 模块实现媒体源加载的**解耦与抽象**：

| 目标 | 说明 |
|------|------|
| 解耦 | 播放器核心不直接处理文件路径 |
| 扩展性 | 支持后续扩展 S3、Azure Blob 等 |
| 协议透明 | 上层代码无需关心源类型 |

### 1.2 架构位置

```
┌─────────────────────────────────────┐
│        播放器核心 (Decoder)          │
└────────────────┬────────────────────┘
                 │ SourceAdapter 接口
                 ▼
┌─────────────────────────────────────┐
│          SourceAdapter (抽象)        │
├───────────┬───────────┬─────────────┤
│ LocalFile │   Http    │   Stream    │
│  Adapter  │  Adapter  │   Adapter   │
└───────────┴───────────┴─────────────┘
```

---

## 2. 类设计

### 2.1 继承层次

```
SourceAdapter (ABC)
    │
    ├── LocalFileAdapter    # 本地文件
    ├── HttpAdapter         # HTTP/HTTPS
    └── StreamAdapter       # RTSP/RTMP/HLS
```

### 2.2 SourceAdapter 接口

| 方法 | 返回类型 | 说明 |
|------|----------|------|
| `validate()` | SourceValidation | 验证源有效性 |
| `get_info()` | SourceInfo | 获取源元信息 |
| `get_url_for_ffmpeg()` | str | 返回 FFmpeg 可用的 URL |
| `get_display_name()` | str | 显示名称 |
| `is_seekable()` | bool | 是否可 seek |
| `get_duration_ms()` | int | 总时长 (需解码后更新) |

### 2.3 数据结构

**SourceInfo**:
| 字段 | 类型 | 说明 |
|------|------|------|
| display_name | str | 显示名称 |
| source_type | SourceType | 源类型枚举 |
| url | str | FFmpeg URL |
| is_seekable | bool | 是否可 seek |
| duration_ms | int | 总时长 (-1 表示未知) |
| file_size | int | 文件大小 (-1 表示未知) |
| mime_type | str? | MIME 类型 |

**SourceType 枚举**:
| 值 | 说明 |
|----|------|
| LOCAL_FILE | 本地文件 |
| HTTP | HTTP 流 |
| HTTPS | HTTPS 流 |
| RTSP | RTSP 流 |
| RTMP | RTMP 流 |
| HLS | HLS (m3u8) |

---

## 3. 适配器规格

### 3.1 LocalFileAdapter

**输入格式**:
- Windows 绝对路径: `C:\Videos\test.mp4`
- Unix 绝对路径: `/home/user/test.mp4`

**验证规则**:
| 检查项 | 失败码 |
|--------|--------|
| 文件存在 | FILE_NOT_FOUND |
| 是文件非目录 | NOT_A_FILE |
| 可读权限 | ACCESS_DENIED |
| 扩展名支持 | UNSUPPORTED_FORMAT |

**支持的扩展名**:
```
视频: .mp4, .mkv, .avi, .mov, .wmv, .flv, .webm, .m4v, .ts, .mts, .m2ts
音频: .mp3, .wav, .flac, .aac, .ogg
```

### 3.2 HttpAdapter

**输入格式**:
- HTTP: `http://example.com/video.mp4`
- HTTPS: `https://example.com/video.mp4`

**特性**:
| 属性 | 值 |
|------|-----|
| seekable | 取决于是否流媒体 |
| 超时 | 可配置 (默认 10s) |

**流媒体检测** (自动判断 seekable):
- URL 包含 `.m3u8` → 不可 seek
- URL 包含 `.mpd` → 不可 seek
- URL 包含 `stream`/`live` → 不可 seek

### 3.3 StreamAdapter

**支持的协议**:
| 协议 | 格式示例 |
|------|----------|
| RTSP | `rtsp://192.168.1.100:554/stream1` |
| RTMP | `rtmp://server/app/stream` |
| HLS | `https://example.com/playlist.m3u8` |

**特性**:
| 属性 | 值 |
|------|-----|
| seekable | False |
| buffer_size | 可配置 (默认 1MB) |

**FFmpeg URL 处理**:
- RTSP 默认添加 `?tcp` 参数优先使用 TCP

---

## 4. 适配器注册表

### 4.1 自动检测逻辑

```
输入字符串
    │
    ├── 以 "rtsp://" 开头 → SourceType.RTSP
    ├── 以 "rtmp://" 开头 → SourceType.RTMP
    ├── 包含 ".m3u8"     → SourceType.HLS
    ├── 以 "https://" 开头 → SourceType.HTTPS
    ├── 以 "http://" 开头  → SourceType.HTTP
    └── 否则              → SourceType.LOCAL_FILE
```

### 4.2 Registry 接口

| 方法 | 说明 |
|------|------|
| `register(type, factory)` | 注册适配器工厂 |
| `detect_type(source)` | 检测源类型 |
| `create(source)` | 创建适配器实例 |
| `create_all(sources)` | 批量创建 |

---

## 5. 使用示例

### 5.1 基本使用

```python
from player.adapters import registry

# 自动检测并创建
adapter = registry.create("C:\\Videos\\test.mp4")
if adapter:
    info = adapter.get_info()
    print(f"Name: {info.display_name}")
    print(f"Seekable: {info.is_seekable}")
```

### 5.2 验证源

```python
def validate_sources(sources):
    errors = []
    for source in sources:
        adapter = registry.create(source)
        if not adapter:
            errors.append(f"无法识别: {source}")
            continue
        result = adapter.validate()
        if not result.is_valid:
            errors.append(f"{source}: {result.error_message}")
    return errors
```

---

## 6. 扩展指南

添加新的适配器 (如 S3):

1. 实现 `SourceAdapter` 接口
2. 添加 `SourceType.S3` 枚举值
3. 注册到 Registry:

```python
registry.register(SourceType.S3, S3Adapter)
```

---

## 7. 目录结构

```
player/adapters/
├── __init__.py      # 模块导出
├── base.py          # SourceAdapter 基类
├── registry.py      # 适配器注册表
├── local_file.py    # 本地文件适配器
├── http_source.py   # HTTP/HTTPS 适配器
└── stream.py        # RTSP/RTMP/HLS 适配器
```

---

## 8. 验收标准

- [ ] 本地文件正确识别并验证
- [ ] HTTP/HTTPS URL 正确处理
- [ ] RTSP/RTMP/HLS 正确处理
- [ ] 不支持的格式返回明确错误
- [ ] 扩展新适配器无需修改核心代码

---

## 9. 依赖关系

**前置**: [native-core.md](native-core.md)
**下一步**: [ui-layer.md](ui-layer.md)
