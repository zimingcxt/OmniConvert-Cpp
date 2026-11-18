# OmniConvert 万能格式工坊 (C++20 / Qt6 跨平台离线便携版)

基于 **C++20 + Qt6** 对原版 Python 工具（已归档于 `legacy/omni_converter.py`）进行完整原生重构，零 Python 运行时依赖，开箱即用，支持 **macOS** 与 **Windows** 完全离线运行。

## 核心功能模块

1. **🖼️ 图片多级压缩与互转**
   - 输入支持：`JPG / JPEG / PNG / WEBP / AVIF / HEIC / HEIF / BMP / TIFF / GIF / ICO`
   - 输出支持：`WEBP / JPG / PNG / AVIF / HEIC / ICO / BMP / TIFF / GIF`
   - 内置纯 C++ **中位切分色彩量化器**（PNG 均衡 256 色 / 极限 128 色，复现 TinyPNG 压缩效果）、**多尺寸图层 `.ico` 生成器**（256/128/64/48/32/16px）、**纯 C++ GIF89a LZW 编码器**、EXIF 方向自动修正与隐私剥离。
2. **🎵 音频转换与视频一键提音**
   - 支持直接拖入 `MP4 / MKV / MOV / AVI / WEBM` 等视频提取音轨，或 `MP3 / M4A / FLAC / WAV / OGG / OPUS` 互转，支持采样率重采样与 EBU R128 (`loudnorm`) 智能响度均衡。
3. **🎬 视频多级瘦身压缩 & 调色盘 GIF**
   - 支持 `MP4 / MKV / MOV / WEBM / GIF / AVI`，支持 `H.264` 与 `H.265 (hvc1)` 编码切换、Retina 奇数像素自动对齐偶数、静音纯画面导出及双通道调色盘高质量动图 GIF 生成。
4. **📚 小说电子书全格式互转与智能排版**
   - 输入支持：`TXT / EPUB / MOBI / AZW / AZW3 / PDF / DOCX / FB2 / MD / HTML / HTM`
   - 输出支持：`EPUB / TXT / MOBI / PDF / HTML / DOCX / MD / FB2`
   - 纯 C++ 原生实现：`GB18030 / GBK / Big5 / UTF-16 / UTF-8` 智能零乱码识别、中英文目录正则切章与超长无章小说自动分节、标准 `EPUB 3` 与 `OpenXML DOCX` 打包、零依赖 `PalmDOC / MOBI6` 解包与 4096 字节 UTF-8 边界安全编码器、`Qt6 QPdfWriter` 多版式排版（A4 / B5 / 6寸电纸书）及单文件离线网页阅读器（支持目录折叠、羊皮纸护眼/夜间深色切换与字号调节）。

## 直接运行（macOS）

当前目录已预编译并封装好独立的 macOS 应用包（已内嵌 Qt6 框架、图像插件与静态离线 `ffmpeg` 内核）：

- 在访达（Finder）中直接双击 **`OmniConvert.app`** 即可离线运行；
- 或在终端执行：`open OmniConvert.app`。

## 跨平台编译打包指南

### 1. macOS 本地编译与独立打包
```bash
chmod +x scripts/build_macos.sh
./scripts/build_macos.sh /path/to/Qt/6.x.x/macos
```
构建完成后会在根目录生成内嵌全部 Qt6 动态库与插件并完成签名的 `OmniConvert.app`。

### 2. Windows (x64 MSVC) 编译与绿色便携包制作
在安装了 Visual Studio 2022、CMake 及 Qt 6（勾选 `Qt Image Formats`）的 Windows 环境中打开 PowerShell：
```powershell
# 若需内置离线音视频内核，可将 ffmpeg.exe 放入 .\bin\ffmpeg.exe
.\scripts\build_windows.ps1 -QtPrefix "C:\Qt\6.7.3\msvc2022_64"
```
脚本将自动完成编译、`windeployqt` 依赖收集并打包生成免安装的绿色便携目录 `dist\OmniConvert-Win64-Portable` 与压缩包 `dist\OmniConvert-Win64-Portable.zip`。

### 3. GitHub Actions 自动双端构建
仓库已内置 `.github/workflows/build-cross-platform.yml`，推送到 GitHub 后可一键同时产出 **macOS 离线应用包** 与 **Windows x64 绿色便携离线包**（自动内置静态 `ffmpeg`）。
