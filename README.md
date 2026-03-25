# VoidPlayer

## 前置依赖

### 运行环境

- **Python 3.11+**

### Native 模块构建

- **CMake** - [下载地址](https://cmake.org/download/)

编译器 (二选一):

- **Visual Studio 2022+** - 需安装 "使用 C++ 的桌面开发" 工作负载
- **MSYS2 UCRT64** - 需安装 `mingw-w64-ucrt-x86_64-gcc` 和 `mingw32-make`

### 打包分发 (可选)

- **Inno Setup 6** - [下载地址](https://jrsoftware.org/isdl.php)

## 构建

### 单步构建
```bash
python build.py all
```

### 分步构建
```bash
# Native 模块 (自动检测编译器)
python build.py native
# 指定编译器
# python build.py native -c msvc    # Visual Studio
# python build.py native -c ucrt64  # MSYS2 UCRT64

# Nuitka 打包
python build.py nuitka

# 打包分发
python build.py package
```
