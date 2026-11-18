<div align="center">

<img src="resources/app_icon.png" width="112" height="112" alt="OmniConvert Logo" />

# OmniConvert 万能格式工坊

**基于 C++20 与 Qt6 构建的跨平台、全离线、零依赖便携式多媒体与电子书转换压缩工具**

</div>

## ✨ 项目简介

**OmniConvert** 是一款专为桌面端打造的**全离线、绿色便携万能格式转换与多级压缩工坊**。本项目采用现代 **C++20** 与 **Qt6** 对原版 Python 工具（归档于 `legacy/omni_converter.py`）进行了彻底重写，摆脱了 Python 解释器与第三方运行库依赖：

- **100% 本地离线处理**：所有图片、音视频与电子书文档均在本地完成编解码与重排版，无需联网，保护文件与隐私安全。
- **开箱即用 · 绿色便携**：发行版已内置 Qt6 核心动态库、图像插件及静态 `FFmpeg` 内核；优先在程序同级目录读写 `omni_settings.json`（只读目录自动回退至 `~/.omniconvert/`），拷入 U 盘或移动硬盘即可随插随用。
- **人性化交互体验**：支持文件/文件夹全局拖拽、**跨模块文件拖入智能识别自动切换分类**、深色/浅色双主题切换、四档直观压缩预设（无损 / 轻度 / 均衡 / 极限）、实时体积节省比计算及双击直接定位输出文件。
- **工业级防丢数保障**：内置 `DualPoolScheduler` 自适应双池并发调度器与 `AtomicFileTx` 原子文件写入事务，任何中途取消或异常均自动回滚清理临时文件，绝不污染或覆盖源文件。

## 🎯 核心功能与支持格式

| 工坊模块 | 支持输入格式 | 支持输出格式 | 核心技术亮点 |
| :--- | :--- | :--- | :--- |
| **🖼️ 图片压缩与转换** | `JPG` `JPEG` `PNG` `WEBP` `AVIF` `HEIC` `HEIF` `BMP` `TIFF` `TIF` `GIF` `ICO` | `WEBP` `JPG` `PNG` `AVIF` `HEIC` `ICO` `BMP` `TIFF` `GIF` | • **纯 C++ 中位切分色彩量化器**：PNG 均衡(256色)/极限(128色)智能减色，复现 TinyPNG 级瘦身<br>• **多尺寸图层 `.ico` 引擎**：自动合成 256/128/64/48/32/16px 标准图标<br>• **EXIF 方向与隐私**：自动旋转手机照片方向，支持一键剥离拍摄参数与 GPS 位置 |
| **🎵 音频转换与提音** | **音频**：`MP3` `WAV` `FLAC` `AAC` `M4A` `OGG` `OPUS` `WMA` `AIFF` `APE` `AMR`<br>**视频提音**：支持直接拖入任意视频提取音轨 | `MP3` `M4A` `FLAC` `WAV` `OGG` `OPUS` | • **视频一键转音频**：直接拖入 `MP4/MKV/MOV` 等视频即可高速抽取纯净音频<br>• **智能响度均衡**：支持开启 EBU R128 (`loudnorm`) 标准响度归一化，统一播客/录音音量<br>• **多级码率与重采样**：支持 48kHz / 44.1kHz / 24kHz 采样率自由切换 |
| **🎬 视频压缩与动图** | `MP4` `MKV` `MOV` `AVI` `WEBM` `FLV` `WMV` `M4V` `TS` `MTS` `MPG` `MPEG` `3GP` | `MP4` `MKV` `MOV` `WEBM` `GIF` `AVI` | • **双编码器支持**：支持 `H.264`（全设备高兼容）与 `H.265 / HEVC`（`hvc1` 标签，Apple 原生可播）<br>• **高画质调色盘 GIF**：采用 `palettegen + paletteuse` 双通道滤镜生成细腻动图<br>• **Retina 录屏防错**：自动对齐偶数像素宽高，支持一键剥离音轨导出静音视频 |
| **📚 小说电子书互转** | `TXT` `EPUB` `MOBI` `AZW` `AZW3` `PDF` `DOCX` `FB2` `MD` `HTML` `HTM` | `EPUB` `TXT` `MOBI` `PDF` `HTML` `DOCX` `MD` `FB2` | • **乱码终结者**：智能识别 `GB18030 / GBK / Big5 / UTF-16 / UTF-8` 编码，老旧 TXT 小说零乱码<br>• **智能分章引擎**：正则识别中英文卷章标题；无目录超长小说按每 160 段平滑切节防卡死<br>• **纯 C++ 零依赖生成**：内置 `EPUB 3`、`OpenXML DOCX`、`PalmDOC/MOBI6`（4096 字节 UTF-8 边界安全编码）、`PDF`（A4 / B5 / 6寸电纸书版式）及**带侧边目录/羊皮纸护眼/夜间模式/字号调节的单文件 HTML 阅读器** |

## 📦 下载与安装

请前往仓库右侧的 **Releases** 页面下载对应操作系统的免安装离线包：

### 🍎 macOS (Apple Silicon / M系列芯片)
1. 下载 **`OmniConvert-macOS-Offline.zip`** 并解压。
2. 双击 **`OmniConvert.app`** 即可直接运行（也可拖入 `/Applications` 应用程序文件夹）。
3. **首次打开提示说明**：若 macOS 弹出“无法验证开发者”提示，可在终端执行以下命令解除隔离属性，或在“系统设置 → 隐私与安全性”中点击“仍要打开”：
   ```bash
   xattr -cr /path/to/OmniConvert.app
   ```

### 🪟 Windows (Windows 10 / 11 x64)
1. 下载 **`OmniConvert-Windows-x64-Offline.zip`** 并解压到任意文件夹（含 U 盘）。
2. 双击目录内的 **`OmniConvert.exe`** 即可直接运行（无需安装 Python、Qt 或配置系统环境变量，静态 `ffmpeg.exe` 已内置于 `./bin/` 目录）。

## 🗂️ 仓库目录结构

```text
OmniConvert-Cpp/
├── CMakeLists.txt                  # 跨平台 CMake 构建主配置 (macOS / Windows / Linux)
├── CMakePresets.json               # macOS 与 Windows MSVC 预设配置
├── vcpkg.json                      # Windows 可选 vcpkg 依赖清单
├── include/omni/                   # 头文件目录
│   ├── core/                       # 核心类型、DualPoolScheduler 双池调度器、Qt 信号槽桥接
│   ├── infra/                      # 便携环境探测、AtomicFileTx 原子文件事务、ZIP 归档、智能编码转换
│   ├── engines/                    # 图片、音视频 (FFmpeg)、小说电子书三大核心引擎接口
│   └── ui/                         # 深浅双主题样式表、拖拽区控件、主窗口头文件
├── src/                            # C++20 / Objective-C++ 核心源码实现
├── resources/                      # 应用图标 (.png / .icns / .ico)、Qt 资源 (.qrc)、Info.plist 与 Windows .rc
├── scripts/
│   ├── build_macos.sh              # macOS 一键编译、CTest 测试、macdeployqt 打包与签名脚本
│   └── build_windows.ps1           # Windows 一键编译、windeployqt 收集与绿色便携包生成脚本
├── tests/
│   └── test_m1_core.cpp            # 端到端自动化单元测试套件 (ZIP/编码/图片/小说回读/音视频/并发调度)
└── legacy/
    └── omni_converter.py           # 原版 Python (PySide6) 参考实现归档
```

## 🛠️ 从源码构建

### 环境要求
- **C++ 编译器**：支持 C++20（macOS Xcode Clang 15+ / Windows MSVC 2022 / Linux GCC 11+）
- **CMake**：3.21 及以上
- **Qt 6 SDK**：Qt 6.5 ~ 6.8（包含 `Core`、`Gui`、`Widgets` 及 `Qt Image Formats` 插件）
- **FFmpeg**（可选，用于音视频转换）：将静态编译的 `ffmpeg`（或 `ffmpeg.exe`）放入项目根目录 `./bin/` 下，构建脚本会自动将其打包进最终产物。

### 1. macOS 编译与 `.app` 独立封包
```bash
chmod +x scripts/build_macos.sh
./scripts/build_macos.sh /path/to/Qt/6.8.3/macos
```
脚本会自动完成 Release 编译、运行 `ctest` 全套单元测试、调用 `macdeployqt` 嵌入框架与图像插件、内置 `./bin/ffmpeg` 并完成 Ad-hoc 签名，最终在根目录产出独立的 **`OmniConvert.app`**。

### 2. Windows (x64 MSVC) 编译与绿色便携包制作
在安装了 Visual Studio 2022 与 Qt 6 的 PowerShell 中执行：
```powershell
.\scripts\build_windows.ps1 -QtPrefix "C:\Qt\6.8.3\msvc2022_64"
```
构建完成后，将在 `dist\OmniConvert-Win64-Portable` 目录及 `dist\OmniConvert-Win64-Portable.zip` 生成包含全部依赖 DLL 的绿色免安装包。

### 3. 运行自动化测试套件
```bash
ctest --test-dir build/macos-release --output-on-failure
```

## ☁️ GitHub Actions 云端自动构建与发版

本仓库已内置双平台 CI/CD 流水线（`.github/workflows/build-cross-platform.yml`）：

- **日常构建**：每次向 `main` 分支推送代码时，自动在云端编译并测试 macOS 与 Windows 双端离线包，产物可在 Actions 页面 **Artifacts** 中下载。
- **一键发布 Release**：
  - **方式一（标签触发）**：在终端执行 `git tag v2.0.0 && git push origin v2.0.0`，流水线将在双端构建通过后自动创建 GitHub Release 并挂载双端 `.zip` 离线包。
  - **方式二（网页手动触发）**：进入仓库 **Actions** 页面 → 选择 **Build Cross-Platform Offline Releases** → 点击 **Run workflow** 并填写版本号（如 `v2.0.0`），即可全自动完成打包与 Release 发布。
