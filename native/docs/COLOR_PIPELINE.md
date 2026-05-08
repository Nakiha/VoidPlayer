# Color Pipeline

VoidPlayer 当前输出目标是 Flutter 暴露的 BGRA/RGB888 SDR surface。native 侧不声明 HDR passthrough，不设置 Windows HDR metadata，也不做 ICC / display profile / per-monitor color management。PQ/HLG 片源会在 shader 中 tone-map 到 SDR 后再写入 BGRA render target。

真 HDR 输出需要 Flutter engine / Windows swapchain / surface format / HDR metadata 级别的改造；在此之前，native 侧的目标是让软件帧和硬件帧在送入 shader 前尽量一致，并由同一套 shader 完成 YUV -> RGB 与 SDR tone mapping。

## Frame Format Policy

FrameConverter 不使用 `libswscale` / `libyuv` 通用 fallback。所有软件像素格式都走显式 packer：

| 输入格式 | 送入 shader 前的格式 | 说明 |
| --- | --- | --- |
| `YUV420P`, `YUVJ420P` | CPU NV12 | U/V 交织成 UV plane |
| `NV12` | CPU NV12 | 直接复制 Y/UV |
| `NV21` | CPU NV12 | VU 交换成 UV |
| `YUV422P`, `YUVJ422P` | CPU NV12 | 垂直 2:1 chroma downsample |
| `YUV444P`, `YUVJ444P` | CPU NV12 | 2x2 chroma downsample |
| `YUV420P10LE` | CPU P010 | 10-bit low-bit planar 转 P010 high-bit layout |
| `P010LE` | CPU P010 | 保留 P010 high-bit layout |
| `YUV422P10LE` | CPU P010 | 垂直 2:1 chroma downsample，保留 10-bit |
| `YUV444P10LE` | CPU P010 | 2x2 chroma downsample，保留 10-bit |

8-bit 软件帧上传为 `DXGI_FORMAT_NV12`，10-bit 软件帧上传为 `DXGI_FORMAT_P010`。硬解 D3D11VA direct path 根据解码 surface 的实际 DXGI format 创建 plane SRV；`NV12` 使用 `R8/R8G8` SRV，`P010/P016` 使用 `R16/R16G16` SRV。

4:2:2 / 4:4:4 软件帧当前会下采样到 4:2:0 后显示。NVIDIA Blackwell 等新硬件可能支持 HEVC/H.264 4:2:2 硬解，但当前 renderer-owned D3D11 direct 上屏路径只接受 NV12/P010/P016 这类 4:2:0 surface。遇到标记为 4:2:2 / 4:4:4 的流时应优先走软件解码，直到 native 增加 `Y210/Y216/Y410/Y416/AYUV` 等 GPU surface 的专用 shader path。

软硬一致性的硬约束是：同一片源如果能同时走软件和硬件路径，送入 shader 前必须同构。当前要求如下：

| 片源/解码输出 | 软件路径送 shader 前 | 硬件 direct 路径送 shader 前 |
| --- | --- | --- |
| 8-bit 4:2:0 | `DXGI_FORMAT_NV12` | `DXGI_FORMAT_NV12` |
| 10-bit 4:2:0 | `DXGI_FORMAT_P010` | `DXGI_FORMAT_P010` / `P016` plane SRV |
| 8/10-bit 4:2:2 | software-only: downsample to NV12/P010 | 当前禁用 renderer-owned direct path，避免把 `Y210/Y216` 等 surface 误采样成 NV12/P010 |
| 8/10-bit 4:4:4 | software-only: downsample to NV12/P010 | 当前禁用 renderer-owned direct path，避免把 `Y410/Y416/AYUV` 等 surface 误采样成 NV12/P010 |

## Color Metadata

FrameConverter 从 `AVFrame` 读取：

| Metadata | 支持值 |
| --- | --- |
| Range | limited, full |
| Matrix | BT.601, BT.709, BT.2020 non-constant-luminance |
| Transfer | SDR, PQ, HLG |
| Primaries | BT.601, BT.709, BT.2020 |

未知 range 默认 limited；`YUVJ420P/YUVJ422P/YUVJ444P` 在 metadata 缺失时默认 full range。未知 matrix 按分辨率推断：宽度 `>=1280` 或高度 `>576` 使用 BT.709，否则 BT.601。未知 transfer 默认 SDR。未知 primaries 根据 matrix 推断。

## Shader Conversion

`shaders/multitrack.hlsl` 负责：

- limited/full range 展开
- BT.601 / BT.709 / BT.2020_NCL YUV -> RGB
- BT.2020 primaries 转 BT.709
- PQ / HLG tone-map 到 SDR/sRGB
- SDR 输出写入 BGRA render target

当前 tone mapping 是播放器预览用的稳定 SDR 映射，不是完整影视级 HDR pipeline。调整曲线时应补 golden/capture 测试，确保软件帧和硬件帧同源输入的 BGRA 输出保持一致。
