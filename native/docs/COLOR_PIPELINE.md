# Color Pipeline

VoidPlayer 当前输出目标是 Flutter 暴露的 BGRA/RGB888 SDR surface。native 侧不声明 HDR passthrough，不设置 Windows HDR metadata，也不做 ICC / display profile / per-monitor color management。PQ/HLG 片源会在 shader 中 tone-map 到 SDR 后再写入 BGRA render target。

当前 render target 使用 `DXGI_FORMAT_B8G8R8A8_UNORM`。shader 写入的是 SDR/sRGB code value，不依赖 `_SRGB` render target 的硬件 gamma encode；也就是说普通 SDR YUV -> RGB 得到的是非线性 R'G'B'，HDR tone-map 分支会先得到线性 BT.709，再显式 `linear_to_srgb()` 后输出。Flutter/DWM 后续如何映射到显示器由系统和 Flutter engine 决定，native 不做 PotPlayer/MPV 那类显示设备 profile 或视频 renderer 级别的色彩管理。

真 HDR 输出需要 Flutter engine / Windows swapchain / surface format / HDR metadata 级别的改造；在此之前，native 侧的目标是让软件帧和硬件帧在送入 shader 前尽量一致，并由同一套 shader 完成 YUV -> RGB 与 SDR tone mapping。

## Frame Format Policy

FrameConverter 不使用 `libswscale` / `libyuv` 通用 fallback。普通 8-bit
4:2:0 软件帧保留原始 plane layout 上传，其余支持格式仍走显式 packer：

| 输入格式 | 送入 shader 前的格式 | 说明 |
| --- | --- | --- |
| `YUV420P`, `YUVJ420P` | CPU planar Y/U/V | 三个 R8 plane texture，shader 直接采样 |
| `NV12` | CPU NV12 | 直接复制 Y/UV |
| `NV21` | CPU NV12 | VU 交换成 UV |
| `YUV422P`, `YUVJ422P` | CPU NV12 | 垂直 2:1 chroma downsample |
| `YUV444P`, `YUVJ444P` | CPU NV12 | 2x2 chroma downsample |
| `YUV420P10LE` | CPU P010 | 10-bit low-bit planar 转 P010 high-bit layout |
| `P010LE` | CPU P010 | 保留 P010 high-bit layout |
| `YUV422P10LE` | CPU P010 | 垂直 2:1 chroma downsample，保留 10-bit |
| `YUV444P10LE` | CPU P010 | 2x2 chroma downsample，保留 10-bit |

8-bit `YUV420P/YUVJ420P` 软件帧上传为三张 `DXGI_FORMAT_R8_UNORM`
texture；8-bit NV12/NV21 和 4:2:2/4:4:4 fallback 上传为
`DXGI_FORMAT_NV12`，10-bit 软件帧上传为 `DXGI_FORMAT_P010`。硬解 D3D11VA
direct path 根据解码 surface 的实际 DXGI format 创建 plane SRV；`NV12` 使用
`R8/R8G8` SRV，`P010/P016` 使用 `R16/R16G16` SRV。

4:2:2 / 4:4:4 软件帧当前会下采样到 4:2:0 后显示。NVIDIA Blackwell 等新硬件可能支持 HEVC/H.264 4:2:2 硬解，但当前 renderer-owned D3D11 direct 上屏路径只接受 NV12/P010/P016 这类 4:2:0 surface。遇到标记为 4:2:2 / 4:4:4 的流时应优先走软件解码，直到 native 增加 `Y210/Y216/Y410/Y416/AYUV` 等 GPU surface 的专用 shader path。

软硬一致性的硬约束是：同一片源如果能同时走软件和硬件路径，送入 shader 前必须同构。当前要求如下：

| 片源/解码输出 | 软件路径送 shader 前 | 硬件 direct 路径送 shader 前 |
| --- | --- | --- |
| 8-bit 4:2:0 | planar `R8` Y/U/V 或 `DXGI_FORMAT_NV12` | `DXGI_FORMAT_NV12` |
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

`shaders/multitrack.hlsl` include 的 `shaders/color_pipeline.hlsl` 负责：

- limited/full range 展开
- BT.601 / BT.709 / BT.2020_NCL YUV -> RGB
- BT.2020 primaries 转 BT.709；BT.601 primaries 当前按 RGB code value 直接输出，不单独做 gamut conversion
- PQ / HLG tone-map 到 SDR/sRGB code value
- SDR 输出写入 BGRA render target

普通 SDR 分支在 YUV -> RGB 后保留一个历史性的 `1/255` 轻微下压，用于维持旧软件解码显示路径的取整侧一致性。它只会造成约 1 个 8-bit code value 的暗部/中间调偏移，不应被当作 HDR 或 full-range/limited-range 的大幅色彩修正。

当前 tone mapping 是播放器预览用的稳定 SDR 映射，不是完整影视级 HDR pipeline。它不读取 mastering display metadata / MaxCLL，不按目标显示器峰值亮度自适应，也不实现 PotPlayer 等播放器可能启用的视频 renderer、ICC 或 GPU driver 级增强。调整曲线时应补 golden/capture 测试，确保软件帧和硬件帧同源输入的 BGRA 输出保持一致。

## MHW Full-Range BT.709 Fixture

`resources/video/mhw_hevc_fullrange_bt709_3s.mp4` 是从本地 Monster Hunter Wilds 4K 样本 stream copy 出来的 portable fixture。`ffprobe` metadata 为：

| 字段 | 值 |
| --- | --- |
| Pixel format | `yuv420p` |
| Range | `pc` / full |
| Matrix | `bt709` |
| Transfer | `bt709` |
| Primaries | `bt709` |
| Resolution | `3840x2160` |

它是 SDR full-range BT.709，不是 HDR/PQ/HLG。`ui_tests/color/hevc_fullrange_bt709_decode_mode_single_track_diff.csv` 用它比较 force software decode 和 preferred hardware decode 的最终 BGRA capture，覆盖 full-range metadata 在软解/硬解路径是否一致。

2026-05-11 手工检查中，`build/color_hevc_full_soft.png` 与 `build/color_hevc_full_hard.png` 完全一致；与 FFmpeg 解出的同帧 1920x1080 RGB 参考相比，平均绝对 RGB 差约 1 code value，平均亮度基本持平。因此如果这条 MHW 样本肉眼比 PotPlayer 更深，优先怀疑 PotPlayer 侧的视频 renderer/range/color-management/enhancement 设置，或 native 缺少显示器 profile 管理，而不是当前 native soft/hard YUV metadata 链路已经把 full-range 当 limited-range。
