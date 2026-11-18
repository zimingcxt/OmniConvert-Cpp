<div align="center">

<img src="resources/app_icon.png" alt="OmniConvert Logo" width="112" height="112" />

# OmniConvert Pro 万能格式工坊 (v3.0)

**基于 C++20 与 Qt6 构建的高自由度、纯本地离线全能格式转换与生活效率桌面工具**

支持 **macOS (Apple Silicon / Intel)** 与 **Windows 10 / 11 (x64)** · 零云端上传 · 开箱即用

</div>

## ✨ 核心特性与设计理念

- **🔒 100% 本地离线与隐私安全**：身份证加水印、合同扫描件转 PDF、私人照片与电子书排版全程在本机离线完成，绝不上传任何第三方服务器。
- **🎛️ 新手省心预设 + 极客像素级自由度**：既提供 `无损原画 / 轻度压缩 / 均衡推荐 / 极限瘦身` 四档一键预设，也提供 **`自定义精细参数`** 模式，允许精确到 `1%` 画质、`1 kbps` 码率、`1 CRF` 因子、字号与行距微调。
- **📚 为小说与深度阅读爱好者量身定制**：内置本地图片自定义封面、程序化典雅书封自动绘制（含书名与作者印章排版）、离线简繁互转、盗版网文广告行自动净化，以及 EPUB / PDF / MOBI / 单文件护眼阅读器全链路书封直嵌。
- **🧰 解决高频生活痛点的效率百宝箱**：内置公务员/教资报名照 **精确卡 KB 二分压缩**、证件复印件 **全屏防盗斜向安全水印**、**Excel 中文防乱码表格互转**、**字幕时间轴校准与台词文稿提取** 以及大文件 **SHA-256 校验清单**。

## 🧩 六大核心功能模块一览

| 模块 | 输入与输出支持 | 精细控制与特色能力 |
| :--- | :--- | :--- |
| **🖼️ 图片转换与精修** | **15+ 种格式**：`WEBP` `JPG` `PNG` `AVIF` `HEIC` `SVG` `ICO` `ICNS` `BMP` `TIFF` `GIF` `TGA` `PPM` `PDF` | `1~100` 自由画质、`2~256` 色调色板量化、按百分比缩放或自定义最大像素边长、`90°/180°/270°` 旋转、水平镜像、黑白文档去色、自定义透明填充底色、右下角署名水印、抹除 EXIF 隐私 |
| **🎵 音频提取与裁剪** | **10 种音频输出**：`MP3` `M4A` `AAC` `FLAC` `WAV` `OGG` `OPUS` `AIFF` `AC3` `WMA`（支持直接拖入视频抽离音轨） | `16~512 kbps` 自定义码率、`8kHz ~ 96kHz` 采样率（含 `16000 Hz` AI 语音识别专用档）、单声道/立体声切换、`0.5x ~ 2.0x` 变速不变调、起止时间铃声裁剪、`EBU R128` 智能响度均衡 |
| **🎬 视频转码与瘦身** | **10 种视频与动图**：`MP4` `MKV` `MOV` `WEBM` `GIF` `APNG` `AVI` `FLV` `TS` `WMV` | `CRF 0~51` 恒定画质控码、`H.264 / H.265 (HEVC)` 编码切换、`5~120 FPS` 帧率控制、自定义像素宽度、起止时间片段无损/重编码截取、一键静音剔除音轨、双遍调色板高清 `GIF` 动图 |
| **📚 小说电子书工坊** | **8 种双向互转**：`TXT` `EPUB` `PDF` `MOBI` `AZW3` `DOCX` `HTML` `MD` `FB2` | **自定义本地图片封面** 或 **一键自动绘制典雅古风/现代书封**（直嵌 EPUB 书架、PDF 扉页、MOBI 封面记录与网页阅读器）、**离线繁简互转**、自动清洗网文网址与求票广告、自定义书名/作者/分章正则、`9~22pt` 字号与行距微调、`GB18030/UTF-8` 智能防乱码 |
| **📄 PDF 与表格工坊** | **PDF 套件**：多图合 PDF、多 PDF 合并、PDF 拆高清图<br>**数据表格**：`CSV` `TSV` `JSON` `Markdown` `HTML` | 支持上下调整扫描件页序、贴合原图无白边或 `A4/B5` 标准页、矢量级无损合并 PDF、`96/150/300 DPI` 导图；表格转换自动修复 `GBK` 乱码并写入 `UTF-8 BOM` 彻底解决 Excel 中文乱码，支持生成带实时搜索框的 HTML 报表 |
| **🧰 生活效率百宝箱** | **四大高频实用工具** | **① 证件照精确卡 KB**：二分搜索画质+阶梯微调尺寸，严格 `≤ 20KB/50KB/100KB`<br>**② 身份证防盗水印**：离线铺满明暗双色抗涂抹斜向水印<br>**③ 字幕互转与校准**：`SRT/VTT/ASS` 毫秒级时间轴平移与 `.txt` 纯台词提取<br>**④ 哈希校验清单**：流式计算大文件 `SHA-256` 与 `MD5` 并导出 Excel 清单 |

## 📦 下载与安装指南

前往本仓库右侧的 **Releases** 页面，下载对应操作系统的最新离线便携压缩包：

### 🍎 macOS 用户 (`OmniConvert-macOS-Offline.zip`)
1. 解压后得到独立的 **`OmniConvert.app`**（已内置 Qt6 图形框架与独立静态 `ffmpeg`，无需安装 Homebrew 或任何依赖）。
2. 将 `OmniConvert.app` 拖入「应用程序 (`/Applications`)」即可双击运行。
3. 若首次打开提示「无法验证开发者」，在终端执行以下命令即可解除隔离标记：
   ```bash
   xattr -cr /Applications/OmniConvert.app
   ```

### 🪟 Windows 用户 (`OmniConvert-Windows-x64-Offline.zip`)
1. 解压压缩包到任意文件夹（纯绿色便携版，无需安装程序，不写注册表）。
2. 双击文件夹内的 **`OmniConvert.exe`** 即可直接使用全部六大模块。

## 🛠️ 本地从源码编译与打包

### 环境要求
- C++20 编译器（macOS Xcode Clang 15+ / Windows MSVC 2022）
- CMake 3.21+
- Qt 6.5+（`Core`, `Gui`, `Widgets` 及 `qtimageformats` 插件）
- 可选：`bin/ffmpeg`（或 `bin/ffmpeg.exe`）用于音视频转码模块

### macOS 一键编译、自测与独立封包
```bash
chmod +x scripts/build_macos.sh
./scripts/build_macos.sh /path/to/Qt/6.8.3/macos
```
脚本将自动完成 Release 编译、运行 40+ 项核心自动化单元测试（`ctest`）、调用 `macdeployqt` 注入独立框架、打包 `bin/ffmpeg` 并完成应用签名。

### Windows (PowerShell + MSVC) 编译与测试
```powershell
cmake -S . -B build/win-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/win-release --config Release --parallel
ctest --test-dir build/win-release -C Release --output-on-failure
```

## 🤖 GitHub Actions 云端双平台自动构建与发版

仓库已内置跨平台 CI/CD 工作流（详见 `.github/workflows/build-cross-platform.yml`）：
- **日常推送 (`git push`)**：每次推送到 `main` 分支都会自动在云端编译 macOS 与 Windows 版本并执行全套自动化测试。
- **发布正式版到 Releases**：
  - **方式一（网页手动触发）**：进入仓库顶部 **Actions** ➔ 选择左侧 **Build Cross-Platform Offline Releases** ➔ 点击右侧 **Run workflow** ➔ 输入版本号（例如 `v3.0.0`）即可自动打包并上架到 **Releases**。
  - **方式二（Git 标签触发）**：
    ```bash
    git tag v3.0.0
    git push origin v3.0.0
    ```

## 📂 项目架构目录

```text
OmniConvert-Cpp/
├── CMakeLists.txt                  # 跨平台 CMake 构建配置 (macOS & Windows MSVC)
├── include/omni/
│   ├── core/                       # 核心类型、精细参数结构体与双池并发调度器
│   ├── infra/                      # 原子文件事务、智能编码识别、原生 ZIP 引擎、便携环境
│   ├── engines/                    # 图像 / 音视频 / 小说电子书 / 文档表格 / 生活百宝箱 六大引擎
│   └── ui/                         # Qt6 深浅双色主题样式与高自由度主窗口
├── src/                            # 核心实现源码与 macOS ImageIO / PDFKit 原生硬件加速层
├── resources/                      # 应用图标 (.icns / .ico / .png) 与平台清单
├── scripts/                        # 本地一键构建与独立 .app 封包脚本
├── tests/                          # 全模块端到端无头自动化测试套件
└── .github/workflows/              # GitHub Actions 双平台自动编译与 Release 发布流水线
```

## 📄 开源许可

本项目基于 MIT License 开源，欢迎自由使用、反馈 Issue 或提交 Pull Request。
