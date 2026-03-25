# VoidPlayer v0.1.0 Release Notes

**发布日期**: 2026-03-26

VoidPlayer 是一个基于 Python 和 FFmpeg 的多轨视频播放器，专为视频编辑和审查工作流程设计。

## 主要功能

### 核心播放能力
- 基于 FFmpeg 的高性能视频解码
- 支持多轨道视频同时播放
- 硬件加速解码支持
- 前后帧精确定位功能

### 渲染引擎
- OpenGL 4.5 现代化渲染架构
- SSBO + 持久化映射技术优化
- QOpenGLWindow 实现大幅优化渲染性能
- D3D11 纹理互操作支持

### 用户界面
- 基于 PySide6-Fluent-Widgets 的现代化 UI
- 进度条支持直接输入时间跳转
- 可自定义窗口副标题
- 主题色彩统一管理

### 快捷键系统
- 可配置的快捷键绑定
- 交互功能注册系统
- 完整的 Mock 测试支持

## 技术亮点

- **Native 模块**: C++ 实现的高性能 FFmpeg 绑定
- **信号总线**: 统一的事件通信机制
- **MSVC/UCRT64**: 支持选择编译器构建 native 模块
- **GPLv3 许可**: 开源协议，社区友好

## 系统要求

- Windows 10/11
- Python 3.10+
- OpenGL 4.5 兼容显卡

## 致谢

感谢所有贡献者和社区的支持。

---

**完整源码**: https://github.com/Nakiha/VoidPlayer
**许可证**: GPLv3
