# Native 模块维护指南

## 红绿灯 TDD 工作流

任何 native 模块的修改必须遵循 Red-Green-Refactor 循环。

### 修改流程

1. RED    — 写失败测试，明确预期行为
2. GREEN  — 最小改动让测试通过
3. REFACTOR — 清理实现，测试保持全绿
4. GREEN ✓ — 全部测试通过后，执行文档同步（见下文）

### 构建与测试命令

```bash
# 完整构建 + 测试
python dev.py test --native-only

# 仅构建 native
python dev.py build --native

# PR 快速 gate
python dev.py gate pr-fast

# Debug 构建
python dev.py build --native --debug
```

`native/build.py` 只保留为 `dev.py` 内部调用的 standalone CMake helper；
日常开发和验证统一走项目根目录的 `dev.py`，避免平台产物目录和依赖检查分叉。

### 测试必须全绿

提交前必须满足：

```bash
python dev.py test --native-only   # 全部 PASS
```

测试文件位于 `native/tests/`，对应关系见 [构建与测试](BUILD_AND_TEST.md)。

---

## 文档同步规则

### 何时更新文档

**测试全绿（GREEN）后**，检查本次修改是否涉及以下变更：

| 变更类型 | 需更新的文档 |
|---------|-------------|
| 新增/删除/重命名源文件 | [ARCHITECTURE.md](ARCHITECTURE.md) 目录结构 |
| 新增/修改 public API | 对应子系统文档的 API 表 |
| 线程模型变更（新增/移除线程） | [THREADING_MODEL.md](THREADING_MODEL.md) |
| 数据格式变更（TextureFrame 等） | [DATA_PIPELINE.md](DATA_PIPELINE.md) |
| Clock / 同步逻辑变更 | [CLOCK_AND_SYNC.md](CLOCK_AND_SYNC.md) |
| 缓冲区大小/状态机变更 | [BUFFERING.md](BUFFERING.md) |
| 解码路径变更（软解/硬解） | [DECODE_PIPELINE.md](DECODE_PIPELINE.md) |
| 像素格式 / 色彩转换 / HDR-SDR 边界变更 | [COLOR_PIPELINE.md](COLOR_PIPELINE.md) |
| Seek 逻辑变更 | [SEEK_STRATEGY.md](SEEK_STRATEGY.md) |
| D3D11 / 着色器变更 | [D3D11_BACKEND.md](D3D11_BACKEND.md)、[WINDOWS_PRESENTATION_BACKEND.md](WINDOWS_PRESENTATION_BACKEND.md) |
| FFI 函数签名变更 | [FFI_AND_BINDINGS.md](FFI_AND_BINDINGS.md) |
| 新增测试/基准/Demo | [BUILD_AND_TEST.md](BUILD_AND_TEST.md) |

### 更新原则

1. **只改受影响的文档** — 不做全局扫描式重写
2. **保持行数限制** — 单文档 < 200 行，超出则拆分
3. **渐进式披露** — ARCHITECTURE.md 只改索引，细节改子文档
4. **验证交叉引用** — 确保文档间的链接仍然正确

### Checklist

每次提交前过一遍：

- [ ] 测试全绿
- [ ] 文档已同步（如涉及上表中的变更类型）
- [ ] 无文档内残留的过时信息
- [ ] ARCHITECTURE.md 索引与实际子文档一致

---

## 文档文件清单

| 文件 | 职责 | 维护频率 |
|------|------|---------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 入口索引 | 文件结构变更时 |
| [THREADING_MODEL.md](THREADING_MODEL.md) | 线程模型 | 线程架构变更时 |
| [DATA_PIPELINE.md](DATA_PIPELINE.md) | 数据流 | 帧格式变更时 |
| [CLOCK_AND_SYNC.md](CLOCK_AND_SYNC.md) | 时钟同步 | 时钟逻辑变更时 |
| [BUFFERING.md](BUFFERING.md) | 缓冲区 | 缓冲策略变更时 |
| [DECODE_PIPELINE.md](DECODE_PIPELINE.md) | 解码管线 | 解码路径变更时 |
| [COLOR_PIPELINE.md](COLOR_PIPELINE.md) | 色彩管线 | 像素格式或色彩转换变更时 |
| [SEEK_STRATEGY.md](SEEK_STRATEGY.md) | Seek 策略 | seek 逻辑变更时 |
| [D3D11_BACKEND.md](D3D11_BACKEND.md) | D3D11 后端 | GPU 相关变更时 |
| [WINDOWS_PRESENTATION_BACKEND.md](WINDOWS_PRESENTATION_BACKEND.md) | Windows 产品上屏、诊断与 fallback 合同 | Windows presentation route 变更时 |
| [FFI_AND_BINDINGS.md](FFI_AND_BINDINGS.md) | FFI 绑定 | API 签名变更时 |
| [BUILD_AND_TEST.md](BUILD_AND_TEST.md) | 构建测试 | 构建/测试变更时 |
| [TEST_MATRIX.md](TEST_MATRIX.md) | 测试 ownership / gate 映射 | 新增、删除或重分类测试时 |
| [TARGET_BOUNDARIES.md](TARGET_BOUNDARIES.md) | CMake target / feature 边界 | target 或 feature option 变更时 |
| [MACOS_READINESS.md](MACOS_READINESS.md) | macOS readiness / release gates | macOS runner、package、release gate 变更时 |
| [MACOS_PRESENTATION_BACKEND.md](MACOS_PRESENTATION_BACKEND.md) | macOS Metal presentation contract | macOS texture / Metal / CVPixelBuffer 路径变更时 |
| [MACOS_HDR_EXPLORATION.md](MACOS_HDR_EXPLORATION.md) | macOS HDR/EDR 路径 | Flutter fork pin、presentation mode、HDR 验证方式变更时 |
| [NATIVE_EVENT_PIPELINE.md](NATIVE_EVENT_PIPELINE.md) | native -> Dart EventChannel 合同 | native event envelope 或 bridge 变更时 |
| [ANALYSIS_MODULE.md](ANALYSIS_MODULE.md) | analysis 模块入口 | analysis FFI、parser、generator 变更时 |
| [ANALYSIS_CACHE.md](ANALYSIS_CACHE.md) | VAC2/VACHUNK cache contract | cache layout、chunk policy、generation contract 变更时 |
| [ANALYSIS_OVERLAY.md](ANALYSIS_OVERLAY.md) | analysis overlay contract | overlay scene、hit-test、renderer integration 变更时 |
| [FFMPEG_ANALYZER.md](FFMPEG_ANALYZER.md) | FFmpeg analyzer toolchain | analyzer build/hook/packaging 变更时 |
| [MACOS_NATIVE_ABI.md](MACOS_NATIVE_ABI.md) | macOS native ABI | macOS C ABI struct/function 变更时 |
| [MAINTENANCE.md](MAINTENANCE.md) | 本文档 | 维护流程变更时 |

`native/docs/` 只保留当前行为和合同。
