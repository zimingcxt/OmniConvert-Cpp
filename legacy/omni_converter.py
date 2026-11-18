#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OmniConvert 万能格式工坊
离线便携 · 跨平台 (Windows / macOS / Linux)
支持：图片多级压缩与互转 | 音频转换与视频提音 | 视频多级压缩与转GIF | 小说电子书互转与智能排版
"""

from __future__ import annotations

import html
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# 核心 GUI 依赖检查
# ---------------------------------------------------------------------------
try:
    from PySide6.QtCore import QSize, Qt, QThread, Signal
    from PySide6.QtGui import QAction, QColor, QDragEnterEvent, QDropEvent, QFont, QKeySequence
    from PySide6.QtWidgets import (
        QAbstractItemView,
        QApplication,
        QButtonGroup,
        QCheckBox,
        QComboBox,
        QFileDialog,
        QFrame,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMenu,
        QMessageBox,
        QProgressBar,
        QPushButton,
        QSizePolicy,
        QStackedWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
except ImportError:
    print(
        "\n[缺少界面依赖] 请先执行以下命令安装依赖后再运行：\n"
        "pip install PySide6 Pillow pillow-heif imageio-ffmpeg pymupdf "
        "EbookLib beautifulsoup4 python-docx mobi charset-normalizer\n"
    )
    sys.exit(1)

# ---------------------------------------------------------------------------
# 可选功能库探测（保证即便部分库未装也可降级优雅运行）
# ---------------------------------------------------------------------------
PIL_AVAILABLE = False
HEIF_AVAILABLE = False
AVIF_AVAILABLE = False
try:
    from PIL import Image, ImageOps, features as pil_features

    PIL_AVAILABLE = True
    AVIF_AVAILABLE = bool(pil_features.check("avif"))
    try:
        import pillow_heif

        pillow_heif.register_heif_opener()
        HEIF_AVAILABLE = True
    except Exception:
        HEIF_AVAILABLE = False
except ImportError:
    pass

PYMUPDF_AVAILABLE = False
PYMUPDF_LOCK = threading.Lock()  # PyMuPDF 非线程安全，使用全局互斥锁保护
try:
    import pymupdf
    PYMUPDF_AVAILABLE = True
except ImportError:
    try:
        import fitz as pymupdf  # 兼容旧版导入名
        PYMUPDF_AVAILABLE = True
    except ImportError:
        pymupdf = None  # type: ignore

EBOOKLIB_AVAILABLE = False
try:
    import ebooklib
    from ebooklib import epub

    EBOOKLIB_AVAILABLE = True
except ImportError:
    pass

BS4_AVAILABLE = False
try:
    from bs4 import BeautifulSoup

    BS4_AVAILABLE = True
except ImportError:
    pass

DOCX_AVAILABLE = False
try:
    import docx

    DOCX_AVAILABLE = True
except ImportError:
    pass

MOBI_UNPACK_AVAILABLE = False
try:
    import mobi as mobi_unpack

    MOBI_UNPACK_AVAILABLE = True
except ImportError:
    pass

CHARSET_NORMALIZER_AVAILABLE = False
try:
    from charset_normalizer import from_bytes as cn_from_bytes

    CHARSET_NORMALIZER_AVAILABLE = True
except ImportError:
    pass


# ===========================================================================
# 便携路径与跨平台系统工具
# ===========================================================================
def get_app_dir() -> Path:
    """获取程序所在目录（兼容源码运行与 PyInstaller 便携打包）"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def get_portable_config_path() -> Path:
    """优先使用程序同级目录保存便携配置；若只读（如 macOS /Applications）则回退用户目录"""
    app_dir = get_app_dir()
    portable_cfg = app_dir / "omni_settings.json"
    try:
        if portable_cfg.exists() and os.access(portable_cfg, os.W_OK):
            return portable_cfg
        test_file = app_dir / f".write_test_{os.getpid()}"
        test_file.write_text("ok", encoding="utf-8")
        test_file.unlink(missing_ok=True)
        return portable_cfg
    except Exception:
        fallback_dir = Path.home() / ".omniconvert"
        fallback_dir.mkdir(parents=True, exist_ok=True)
        return fallback_dir / "omni_settings.json"


def ensure_executable(bin_path: Path) -> bool:
    """跨平台修正便携 U 盘拷贝后可能丢失的执行权限"""
    if not bin_path.is_file():
        return False
    if sys.platform != "win32" and not os.access(bin_path, os.X_OK):
        try:
            bin_path.chmod(bin_path.stat().st_mode | 0o111)
        except Exception:
            pass
    return os.access(bin_path, os.X_OK)


def resolve_ffmpeg() -> Optional[str]:
    """三级离线定位 FFmpeg：本地便携 ./bin -> imageio-ffmpeg 内置二进制 -> 系统 PATH"""
    app_dir = get_app_dir()
    exe_name = "ffmpeg.exe" if sys.platform == "win32" else "ffmpeg"
    for candidate in (app_dir / "bin" / exe_name, app_dir / exe_name):
        if ensure_executable(candidate):
            return str(candidate)
    try:
        import imageio_ffmpeg

        exe = imageio_ffmpeg.get_ffmpeg_exe()
        if exe and ensure_executable(Path(exe)):
            return exe
    except Exception:
        pass
    return shutil.which("ffmpeg")


def resolve_calibre_convert() -> Optional[str]:
    """探测可选的 Calibre ebook-convert 命令行工具（存在则增强 AZW3/MOBI 输出）"""
    exe = shutil.which("ebook-convert")
    if exe:
        return exe
    candidates = []
    if sys.platform == "darwin":
        candidates.append(Path("/Applications/calibre.app/Contents/MacOS/ebook-convert"))
    elif sys.platform == "win32":
        for env_key in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
            base = os.environ.get(env_key)
            if base:
                candidates.append(Path(base) / "Calibre2" / "ebook-convert.exe")
    for c in candidates:
        if c.is_file():
            return str(c)
    return None


def reveal_in_file_manager(target: Path) -> None:
    """跨平台打开文件夹并高亮定位生成的文件"""
    if not target.exists():
        target = target.parent
    if not target.exists():
        return
    try:
        if sys.platform == "win32":
            if target.is_file():
                subprocess.Popen(["explorer", f"/select,{os.path.normpath(str(target))}"])
            else:
                os.startfile(str(target))  # type: ignore[attr-defined]
        elif sys.platform == "darwin":
            if target.is_file():
                subprocess.Popen(["open", "-R", str(target)])
            else:
                subprocess.Popen(["open", str(target)])
        else:
            folder = target.parent if target.is_file() else target
            subprocess.Popen(["xdg-open", str(folder)])
    except Exception:
        pass


def format_size(num_bytes: int) -> str:
    """格式化字节大小为易读字符串"""
    if num_bytes < 1024:
        return f"{num_bytes} B"
    val = float(num_bytes)
    for unit in ("KB", "MB", "GB", "TB"):
        val /= 1024.0
        if val < 1024.0 or unit == "TB":
            return f"{val:.2f} {unit}" if val < 100 else f"{val:.1f} {unit}"
    return f"{num_bytes} B"


def resolve_safe_output_path(
    in_path: Path, out_dir: Optional[Path], target_ext: str, suffix_hint: str = ""
) -> Path:
    """生成绝不误覆盖源文件的安全输出路径"""
    ext = target_ext.lower()
    if not ext.startswith("."):
        ext = "." + ext
    folder = out_dir if out_dir else in_path.parent
    folder.mkdir(parents=True, exist_ok=True)

    stem = in_path.stem
    candidate = folder / f"{stem}{ext}"
    if candidate.resolve() == in_path.resolve() or candidate.exists():
        tag = suffix_hint or "_converted"
        candidate = folder / f"{stem}{tag}{ext}"
        idx = 1
        while candidate.resolve() == in_path.resolve() or candidate.exists():
            candidate = folder / f"{stem}{tag}_{idx}{ext}"
            idx += 1
    return candidate


# ===========================================================================
# 分类定义与常量
# ===========================================================================
MODE_IMAGE = "image"
MODE_AUDIO = "audio"
MODE_VIDEO = "video"
MODE_NOVEL = "novel"

IMAGE_IN_EXTS = {
    ".jpg", ".jpeg", ".png", ".webp", ".avif", ".heic", ".heif",
    ".bmp", ".tiff", ".tif", ".gif", ".ico",
}
AUDIO_ONLY_EXTS = {
    ".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg", ".opus", ".wma", ".aiff", ".ape", ".amr",
}
VIDEO_IN_EXTS = {
    ".mp4", ".mkv", ".mov", ".avi", ".webm", ".flv", ".wmv", ".m4v", ".ts", ".mts", ".mpg", ".mpeg", ".3gp",
}
# 音频模式额外支持直接拖入视频提取音轨
AUDIO_IN_EXTS = AUDIO_ONLY_EXTS | VIDEO_IN_EXTS
NOVEL_IN_EXTS = {
    ".txt", ".epub", ".mobi", ".azw", ".azw3", ".pdf", ".docx", ".fb2", ".md", ".html", ".htm",
}

COMPRESSION_PRESETS = [
    ("lossless", "无损 / 原画原音（仅封装转换或最高品质）"),
    ("light", "轻度压缩 · 高品质（几乎无损观感，省 25%~45% 空间）"),
    ("balanced", "均衡压缩 · 推荐（画质与体积黄金平衡，省 50%~70% 空间）"),
    ("extreme", "极限压缩 · 最小体积（适合网传/归档，省 70%~88% 空间）"),
]


def detect_category_for_file(path: Path) -> Optional[str]:
    ext = path.suffix.lower()
    if ext in IMAGE_IN_EXTS:
        return MODE_IMAGE
    if ext in AUDIO_ONLY_EXTS:
        return MODE_AUDIO
    if ext in VIDEO_IN_EXTS:
        return MODE_VIDEO
    if ext in NOVEL_IN_EXTS:
        return MODE_NOVEL
    return None


def is_file_accepted_in_mode(path: Path, mode: str) -> bool:
    ext = path.suffix.lower()
    if mode == MODE_IMAGE:
        return ext in IMAGE_IN_EXTS
    if mode == MODE_AUDIO:
        return ext in AUDIO_IN_EXTS
    if mode == MODE_VIDEO:
        return ext in VIDEO_IN_EXTS
    if mode == MODE_NOVEL:
        return ext in NOVEL_IN_EXTS
    return False


# ===========================================================================
# 引擎一：图片多级压缩与格式转换
# ===========================================================================
def convert_image(
    in_path: Path,
    out_path: Path,
    target_fmt: str,
    comp_level: str,
    max_dim: int = 0,
    strip_exif: bool = False,
    progress_cb: Optional[Callable[[int], None]] = None,
) -> None:
    if not PIL_AVAILABLE:
        raise RuntimeError("未检测到 Pillow 库，请运行 pip install Pillow")

    fmt_upper = target_fmt.upper()
    if (in_path.suffix.lower() in {".heic", ".heif"} or fmt_upper == "HEIC") and not HEIF_AVAILABLE:
        raise RuntimeError("转换 HEIC 格式需安装插件：pip install pillow-heif")
    if (in_path.suffix.lower() == ".avif" or fmt_upper == "AVIF") and not AVIF_AVAILABLE:
        raise RuntimeError("当前 Pillow 版本不支持 AVIF，请升级：pip install -U Pillow")

    if progress_cb:
        progress_cb(15)

    with Image.open(in_path) as raw_img:
        raw_exif = raw_img.info.get("exif")
        icc_profile = raw_img.info.get("icc_profile")
        # 自动根据手机拍摄方向翻转像素，防止剥离 EXIF 后照片横置
        img = ImageOps.exif_transpose(raw_img)

        if progress_cb:
            progress_cb(35)

        if max_dim > 0 and max(img.size) > max_dim:
            img = img.copy()
            img.thumbnail((max_dim, max_dim), Image.Resampling.LANCZOS)

        quality_map = {
            "lossless": 96,
            "light": 85,
            "balanced": 72,
            "extreme": 50,
        }
        q = quality_map.get(comp_level, 75)

        save_kwargs: Dict[str, object] = {}
        if not strip_exif and raw_exif and fmt_upper in {"JPG", "JPEG", "WEBP", "AVIF", "TIFF"}:
            save_kwargs["exif"] = raw_exif
        if icc_profile and fmt_upper in {"JPG", "JPEG", "PNG", "WEBP", "AVIF", "TIFF"}:
            save_kwargs["icc_profile"] = icc_profile

        if fmt_upper == "ICO":
            # Windows / macOS 图标最大 256x256，自动生成多分辨率层
            ico_img = img.convert("RGBA")
            if max(ico_img.size) > 256:
                ico_img.thumbnail((256, 256), Image.Resampling.LANCZOS)
            max_side = max(ico_img.size)
            sizes = [(s, s) for s in (256, 128, 64, 48, 32, 16) if s <= max_side]
            if not sizes:
                sizes = [(32, 32)]
            if progress_cb:
                progress_cb(75)
            ico_img.save(out_path, format="ICO", sizes=sizes)
            if progress_cb:
                progress_cb(100)
            return

        # 对不支持透明通道的目标格式自动垫白底合成
        has_alpha = img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info)
        if fmt_upper in {"JPG", "JPEG", "BMP"}:
            if has_alpha:
                rgba = img.convert("RGBA")
                bg = Image.new("RGB", rgba.size, (255, 255, 255))
                bg.paste(rgba, mask=rgba.split()[3])
                img = bg
            elif img.mode != "RGB":
                img = img.convert("RGB")

        if progress_cb:
            progress_cb(65)

        if fmt_upper in {"JPG", "JPEG"}:
            save_kwargs.update({"quality": q, "optimize": True})
            if comp_level == "lossless":
                save_kwargs["subsampling"] = 0
            img.save(out_path, format="JPEG", **save_kwargs)

        elif fmt_upper == "PNG":
            # PNG 为无损格式，均衡与极限档启用色彩量化（类似 TinyPNG 原理）大幅缩减体积
            out_img = img
            if comp_level in {"balanced", "extreme"}:
                colors = 256 if comp_level == "balanced" else 128
                work = out_img.convert("RGBA" if has_alpha else "RGB")
                out_img = work.quantize(colors=colors, method=Image.Quantize.FASTOCTREE)
            out_img.save(out_path, format="PNG", optimize=True, compress_level=9, **save_kwargs)

        elif fmt_upper == "WEBP":
            if comp_level == "lossless":
                save_kwargs.update({"lossless": True, "quality": 100, "method": 6})
            else:
                save_kwargs.update({"quality": q, "method": 6})
            img.save(out_path, format="WEBP", **save_kwargs)

        elif fmt_upper == "AVIF":
            avif_q = 95 if comp_level == "lossless" else q
            if img.mode not in ("RGB", "RGBA"):
                img = img.convert("RGBA" if has_alpha else "RGB")
            save_kwargs.update({"quality": avif_q, "speed": 6})
            img.save(out_path, format="AVIF", **save_kwargs)

        elif fmt_upper == "HEIC":
            heic_q = -1 if comp_level == "lossless" else q
            if img.mode not in ("RGB", "RGBA"):
                img = img.convert("RGBA" if has_alpha else "RGB")
            img.save(out_path, format="HEIF", quality=heic_q)

        elif fmt_upper == "TIFF":
            tiff_comp = "tiff_lzw" if comp_level in {"lossless", "light"} else "tiff_adobe_deflate"
            img.save(out_path, format="TIFF", compression=tiff_comp, **save_kwargs)

        elif fmt_upper == "GIF":
            colors = 256 if comp_level in {"lossless", "light"} else (128 if comp_level == "balanced" else 64)
            gif_img = img.convert("RGBA" if has_alpha else "RGB").quantize(
                colors=colors, method=Image.Quantize.FASTOCTREE
            )
            gif_img.save(out_path, format="GIF", optimize=True)

        elif fmt_upper == "BMP":
            img.save(out_path, format="BMP")

        else:
            img.save(out_path)

    if progress_cb:
        progress_cb(100)


# ===========================================================================
# 引擎二 & 三：FFmpeg 跨平台进程调度（音频与视频）
# ===========================================================================
DURATION_RE = re.compile(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)")
OUT_TIME_US_RE = re.compile(r"out_time_us=(\d+)")
OUT_TIME_STR_RE = re.compile(r"out_time=(\d+):(\d+):(\d+(?:\.\d+)?)")


def run_ffmpeg_command(
    args: List[str],
    out_path: Path,
    progress_cb: Optional[Callable[[int], None]] = None,
    is_cancelled: Optional[Callable[[], bool]] = None,
) -> None:
    ffmpeg_exe = resolve_ffmpeg()
    if not ffmpeg_exe:
        raise RuntimeError("未找到离线 FFmpeg 内核，请安装 imageio-ffmpeg 或将 ffmpeg 放入 ./bin 目录")

    full_cmd = [ffmpeg_exe, "-y", "-hide_banner", *args, "-progress", "pipe:1", "-nostats", str(out_path)]
    creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

    proc = subprocess.Popen(
        full_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
        creationflags=creationflags,
    )

    total_seconds = 0.0
    tail_logs: List[str] = []

    try:
        assert proc.stdout is not None
        for raw_line in proc.stdout:
            if is_cancelled and is_cancelled():
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                out_path.unlink(missing_ok=True)
                raise InterruptedError("用户已中止转换")

            line = raw_line.strip()
            if not line:
                continue
            tail_logs.append(line)
            if len(tail_logs) > 30:
                tail_logs.pop(0)

            if total_seconds <= 0:
                m_dur = DURATION_RE.search(line)
                if m_dur:
                    h, m, s = int(m_dur.group(1)), int(m_dur.group(2)), float(m_dur.group(3))
                    total_seconds = max(0.1, h * 3600 + m * 60 + s)

            if total_seconds > 0 and progress_cb:
                cur_sec = -1.0
                m_us = OUT_TIME_US_RE.match(line)
                if m_us:
                    cur_sec = int(m_us.group(1)) / 1_000_000.0
                else:
                    m_ts = OUT_TIME_STR_RE.match(line)
                    if m_ts:
                        cur_sec = (
                            int(m_ts.group(1)) * 3600
                            + int(m_ts.group(2)) * 60
                            + float(m_ts.group(3))
                        )
                if cur_sec >= 0:
                    pct = min(99, max(1, int(cur_sec / total_seconds * 100)))
                    progress_cb(pct)

        ret = proc.wait()
        if ret != 0:
            out_path.unlink(missing_ok=True)
            err_detail = "\n".join(tail_logs[-6:])
            raise RuntimeError(f"FFmpeg 编码失败 (退出码 {ret})：\n{err_detail}")

        if progress_cb:
            progress_cb(100)
    except Exception:
        if proc.poll() is None:
            proc.kill()
        raise


def convert_audio(
    in_path: Path,
    out_path: Path,
    target_fmt: str,
    comp_level: str,
    sample_rate: int = 0,
    loudnorm: bool = False,
    progress_cb: Optional[Callable[[int], None]] = None,
    is_cancelled: Optional[Callable[[], bool]] = None,
) -> None:
    fmt = target_fmt.lower()
    args = ["-i", str(in_path), "-vn"]

    if loudnorm:
        args.extend(["-af", "loudnorm=I=-16:TP=-1.5:LRA=11"])

    if sample_rate > 0 and fmt != "opus":
        args.extend(["-ar", str(sample_rate)])

    if fmt == "mp3":
        br = {"lossless": "320k", "light": "192k", "balanced": "128k", "extreme": "64k"}.get(comp_level, "128k")
        args.extend(["-c:a", "libmp3lame", "-b:a", br])
    elif fmt == "m4a":
        br = {"lossless": "256k", "light": "192k", "balanced": "128k", "extreme": "64k"}.get(comp_level, "128k")
        args.extend(["-c:a", "aac", "-b:a", br, "-movflags", "+faststart"])
    elif fmt == "flac":
        lvl = {"lossless": "5", "light": "8", "balanced": "12", "extreme": "12"}.get(comp_level, "8")
        args.extend(["-c:a", "flac", "-compression_level", lvl])
        if comp_level == "extreme":
            args.extend(["-sample_fmt", "s16"])
    elif fmt == "wav":
        args.extend(["-c:a", "pcm_s16le"])
        if comp_level == "extreme" and sample_rate == 0:
            args.extend(["-ar", "22050", "-ac", "1"])
    elif fmt == "ogg":
        q = {"lossless": "9", "light": "6", "balanced": "4", "extreme": "1"}.get(comp_level, "4")
        args.extend(["-c:a", "libvorbis", "-q:a", q])
    elif fmt == "opus":
        br = {"lossless": "192k", "light": "128k", "balanced": "96k", "extreme": "48k"}.get(comp_level, "96k")
        args.extend(["-c:a", "libopus", "-b:a", br])
    else:
        raise ValueError(f"不支持的音频输出格式: {target_fmt}")

    run_ffmpeg_command(args, out_path, progress_cb=progress_cb, is_cancelled=is_cancelled)


def convert_video(
    in_path: Path,
    out_path: Path,
    target_fmt: str,
    comp_level: str,
    max_width: int = 0,
    codec_pref: str = "h264",
    mute_audio: bool = False,
    progress_cb: Optional[Callable[[int], None]] = None,
    is_cancelled: Optional[Callable[[], bool]] = None,
) -> None:
    fmt = target_fmt.lower()
    args = ["-i", str(in_path)]

    # 视频转高质量调色盘动图 GIF
    if fmt == "gif":
        gif_cfg = {
            "lossless": (15, 800, 256),
            "light": (12, 640, 256),
            "balanced": (10, 480, 128),
            "extreme": (8, 360, 64),
        }
        fps, def_w, max_colors = gif_cfg.get(comp_level, (10, 480, 128))
        target_w = min(max_width, def_w) if max_width > 0 else def_w
        vf = (
            f"fps={fps},scale='min({target_w},iw)':-1:flags=lanczos,"
            f"split[s0][s1];[s0]palettegen=max_colors={max_colors}[p];[s1][p]paletteuse=dither=bayer"
        )
        args.extend(["-vf", vf, "-an"])
        run_ffmpeg_command(args, out_path, progress_cb=progress_cb, is_cancelled=is_cancelled)
        return

    # 确保宽高为偶数，防止 macOS Retina 奇数像素录屏在 H.264/H.265 下报错
    if max_width > 0:
        vf_scale = f"scale='trunc(min({max_width},iw)/2)*2':-2"
    else:
        vf_scale = "scale=trunc(iw/2)*2:trunc(ih/2)*2"
    args.extend(["-vf", vf_scale])

    if fmt == "webm":
        crf = {"lossless": "18", "light": "26", "balanced": "32", "extreme": "38"}.get(comp_level, "32")
        args.extend(["-c:v", "libvpx-vp9", "-crf", crf, "-b:v", "0", "-row-mt", "1"])
        if mute_audio:
            args.append("-an")
        else:
            abr = {"lossless": "192k", "light": "128k", "balanced": "96k", "extreme": "64k"}.get(comp_level, "96k")
            args.extend(["-c:a", "libopus", "-b:a", abr])

    elif fmt == "avi":
        qv = {"lossless": "2", "light": "5", "balanced": "9", "extreme": "15"}.get(comp_level, "8")
        args.extend(["-c:v", "mpeg4", "-q:v", qv])
        if mute_audio:
            args.append("-an")
        else:
            args.extend(["-c:a", "libmp3lame", "-b:a", "128k"])

    else:
        # MP4 / MKV / MOV
        if codec_pref == "h265":
            crf = {"lossless": "20", "light": "25", "balanced": "30", "extreme": "35"}.get(comp_level, "28")
            preset = "medium" if comp_level in {"lossless", "light"} else "faster"
            args.extend(["-c:v", "libx265", "-crf", crf, "-preset", preset, "-pix_fmt", "yuv420p"])
            if fmt in {"mp4", "mov"}:
                # 添加 hvc1 标签以确保 macOS QuickTime / 预览 原生可播
                args.extend(["-tag:v", "hvc1"])
        else:
            crf = {"lossless": "18", "light": "23", "balanced": "28", "extreme": "33"}.get(comp_level, "26")
            preset = "medium" if comp_level in {"lossless", "light"} else "faster"
            args.extend(["-c:v", "libx264", "-crf", crf, "-preset", preset, "-pix_fmt", "yuv420p"])

        if mute_audio:
            args.append("-an")
        else:
            abr = {"lossless": "256k", "light": "192k", "balanced": "128k", "extreme": "96k"}.get(comp_level, "128k")
            args.extend(["-c:a", "aac", "-b:a", abr])

        if fmt in {"mp4", "mov"}:
            args.extend(["-movflags", "+faststart"])

    run_ffmpeg_command(args, out_path, progress_cb=progress_cb, is_cancelled=is_cancelled)


# ===========================================================================
# 引擎四：小说电子书智能解析、分章与全格式互转
# ===========================================================================
@dataclass
class Chapter:
    title: str
    paragraphs: List[str] = field(default_factory=list)


@dataclass
class BookDocument:
    title: str
    author: str
    chapters: List[Chapter] = field(default_factory=list)


CHAPTER_TITLE_RE = re.compile(
    r"^\s*(?:正文\s*)?(?:"
    r"第[零一二三四五六七八九十百千万两0-9]+[章节回卷集部篇幕]"
    r"|卷[零一二三四五六七八九十百千万两0-9]+"
    r"|楔子|序章|序言|引子|前言|后记|尾声|番外|终章"
    r"|Chapter\s+\d+|CHAPTER\s+\d+|Part\s+\d+|Book\s+\d+"
    r")(?:[\s：:、.\-—_].*)?$"
)


def clean_book_title_from_stem(stem: str) -> str:
    cleaned = stem.strip()
    m = re.match(r"^《(.+)》$", cleaned)
    if m:
        cleaned = m.group(1).strip()
    return cleaned or "未命名书籍"


def read_text_smart(path: Path) -> str:
    """智能识别中文与多国编码（解决老旧 TXT 小说 GBK/GB18030/Big5/UTF-16 乱码问题）"""
    raw = path.read_bytes()
    if not raw:
        return ""

    # BOM 优先精确判定
    for bom, enc in (
        (b"\xef\xbb\xbf", "utf-8-sig"),
        (b"\xff\xfe", "utf-16-le"),
        (b"\xfe\xff", "utf-16-be"),
    ):
        if raw.startswith(bom):
            return raw.decode(enc, errors="replace")

    # 严格尝试 UTF-8 与中文超集 GB18030
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        pass

    if CHARSET_NORMALIZER_AVAILABLE:
        best = cn_from_bytes(raw).best()
        if best is not None:
            return str(best)

    for enc in ("gb18030", "big5", "utf-16"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue

    return raw.decode("utf-8", errors="replace")


def normalize_paragraph_line(line: str) -> str:
    # 剥离段首原有的全角/半角空白，统一由输出阶段决定缩进
    return line.lstrip(" \t\u3000\xa0").rstrip()


def split_text_into_chapters(raw_text: str, auto_split_long: bool = True) -> List[Chapter]:
    lines = raw_text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    chapters: List[Chapter] = []
    current_title = "序言 / 开始"
    current_paras: List[str] = []
    found_explicit_chapter = False

    for raw_line in lines:
        stripped = raw_line.strip()
        if not stripped:
            continue

        if len(stripped) <= 68 and CHAPTER_TITLE_RE.match(stripped):
            if current_paras:
                chapters.append(Chapter(title=current_title, paragraphs=current_paras))
                current_paras = []
            current_title = stripped
            found_explicit_chapter = True
        else:
            norm = normalize_paragraph_line(stripped)
            if norm:
                current_paras.append(norm)

    if current_paras:
        if not found_explicit_chapter and not chapters:
            current_title = "正文"
        chapters.append(Chapter(title=current_title, paragraphs=current_paras))

    # 若长篇 TXT 无任何章节标题，按每 160 段平滑切分，避免阅读器单章几百万字卡死
    if auto_split_long and len(chapters) == 1 and not found_explicit_chapter:
        all_paras = chapters[0].paragraphs
        if len(all_paras) > 220:
            chunk_size = 160
            chapters = [
                Chapter(
                    title=f"第 {idx + 1} 节",
                    paragraphs=all_paras[i : i + chunk_size],
                )
                for idx, i in enumerate(range(0, len(all_paras), chunk_size))
            ]

    if not chapters:
        chapters.append(Chapter(title="正文", paragraphs=["（空白文档）"]))
    return chapters


def extract_chapters_from_html_string(html_text: str, fallback_title: str = "章节") -> List[Chapter]:
    if not BS4_AVAILABLE:
        plain = re.sub(r"<[^>]+>", "\n", html_text)
        return split_text_into_chapters(html.unescape(plain))

    soup = BeautifulSoup(html_text, "html.parser")
    for tag in soup(["script", "style", "nav", "noscript"]):
        tag.decompose()

    # 将 <br> 替换为换行符以兼容非标准排版电子书
    for br in soup.find_all("br"):
        br.replace_with("\n")

    chapters: List[Chapter] = []
    cur_title = fallback_title
    cur_paras: List[str] = []
    has_heading = False

    body = soup.body if soup.body else soup
    block_tags = body.find_all(["h1", "h2", "h3", "p", "div", "blockquote", "li"])

    if not block_tags:
        return split_text_into_chapters(body.get_text("\n"))

    for el in block_tags:
        # 跳过包含子级块元素的容器 div，防止文本重复提取
        if el.name == "div" and el.find(["h1", "h2", "h3", "p", "div", "blockquote", "li"]):
            continue

        txt = el.get_text("\n").strip()
        if not txt:
            continue

        if el.name in ("h1", "h2", "h3") or (len(txt) <= 68 and "\n" not in txt and CHAPTER_TITLE_RE.match(txt)):
            if cur_paras:
                chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
                cur_paras = []
            cur_title = " ".join(txt.split())
            has_heading = True
        else:
            for sub_line in txt.split("\n"):
                s = normalize_paragraph_line(sub_line)
                if s:
                    if len(s) <= 68 and CHAPTER_TITLE_RE.match(s):
                        if cur_paras:
                            chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
                            cur_paras = []
                        cur_title = s
                        has_heading = True
                    else:
                        cur_paras.append(s)

    if cur_paras:
        chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))

    if not chapters and not has_heading:
        return split_text_into_chapters(body.get_text("\n"))
    return chapters


def parse_with_pymupdf(in_path: Path) -> BookDocument:
    if not PYMUPDF_AVAILABLE:
        raise RuntimeError("未安装 PyMuPDF 库，请运行 pip install pymupdf")

    title = clean_book_title_from_stem(in_path.stem)
    author = "佚名"

    with PYMUPDF_LOCK:
        doc = pymupdf.open(str(in_path))
        try:
            meta = doc.metadata or {}
            if meta.get("title") and meta["title"].strip():
                title = meta["title"].strip()
            if meta.get("author") and meta["author"].strip():
                author = meta["author"].strip()

            toc = doc.get_toc()
            top_toc = [(t[1].strip(), max(0, t[2] - 1)) for t in toc if t[0] == 1 and t[1].strip()]

            if len(top_toc) >= 2 and doc.page_count > 1:
                chapters: List[Chapter] = []
                # 封面与前言页
                if top_toc[0][1] > 0:
                    intro_txt = "\n".join(doc[p].get_text("text") for p in range(0, top_toc[0][1]))
                    intro_paras = [normalize_paragraph_line(x) for x in intro_txt.splitlines() if x.strip()]
                    if intro_paras:
                        chapters.append(Chapter(title="前言", paragraphs=intro_paras))

                for idx, (ch_title, start_p) in enumerate(top_toc):
                    end_p = top_toc[idx + 1][1] if idx + 1 < len(top_toc) else doc.page_count
                    end_p = max(start_p + 1, min(doc.page_count, end_p))
                    chunk_txt = "\n".join(doc[p].get_text("text") for p in range(start_p, end_p))
                    paras = [
                        normalize_paragraph_line(x)
                        for x in chunk_txt.splitlines()
                        if x.strip() and x.strip() != ch_title
                    ]
                    if paras:
                        chapters.append(Chapter(title=ch_title, paragraphs=paras))
                if chapters:
                    return BookDocument(title=title, author=author, chapters=chapters)

            full_text = "\n".join(page.get_text("text") for page in doc)
        finally:
            doc.close()

    return BookDocument(title=title, author=author, chapters=split_text_into_chapters(full_text))


def parse_epub_file(in_path: Path) -> BookDocument:
    if not EBOOKLIB_AVAILABLE:
        return parse_with_pymupdf(in_path)

    try:
        book = epub.read_epub(str(in_path), {"ignore_ncx": True})
        title = clean_book_title_from_stem(in_path.stem)
        author = "佚名"

        t_meta = book.get_metadata("DC", "title")
        if t_meta and t_meta[0][0]:
            title = str(t_meta[0][0]).strip()
        a_meta = book.get_metadata("DC", "creator")
        if a_meta and a_meta[0][0]:
            author = str(a_meta[0][0]).strip()

        doc_items = []
        seen_ids = set()
        for item_id, _ in book.spine:
            it = book.get_item_with_id(item_id)
            if it is not None and it.get_type() == ebooklib.ITEM_DOCUMENT:
                doc_items.append(it)
                seen_ids.add(it.get_id())
        for it in book.get_items_of_type(ebooklib.ITEM_DOCUMENT):
            if it.get_id() not in seen_ids:
                doc_items.append(it)

        chapters: List[Chapter] = []
        for idx, item in enumerate(doc_items, start=1):
            raw_bytes = item.get_content()
            html_str = raw_bytes.decode("utf-8", errors="replace")
            extracted = extract_chapters_from_html_string(html_str, fallback_title=f"第 {idx} 章")
            chapters.extend(extracted)

        if chapters:
            return BookDocument(title=title, author=author, chapters=chapters)
    except Exception:
        pass

    return parse_with_pymupdf(in_path)


def parse_mobi_or_azw3(in_path: Path) -> BookDocument:
    default_title = clean_book_title_from_stem(in_path.stem)
    if MOBI_UNPACK_AVAILABLE:
        temp_dir = None
        try:
            temp_dir, extracted_file = mobi_unpack.extract(str(in_path))
            ext_path = Path(extracted_file)
            doc = parse_novel_to_document(ext_path)
            if doc.title in {"未命名书籍", ext_path.stem} or doc.title.startswith("mobi"):
                doc.title = default_title
            return doc
        except Exception:
            pass
        finally:
            if temp_dir:
                shutil.rmtree(temp_dir, ignore_errors=True)

    return parse_with_pymupdf(in_path)


def parse_docx_file(in_path: Path) -> BookDocument:
    if not DOCX_AVAILABLE:
        raise RuntimeError("读取 DOCX 需安装库：pip install python-docx")

    d = docx.Document(str(in_path))
    title = d.core_properties.title or clean_book_title_from_stem(in_path.stem)
    author = d.core_properties.author or "佚名"

    chapters: List[Chapter] = []
    cur_title = "正文"
    cur_paras: List[str] = []

    for p in d.paragraphs:
        txt = p.text.strip()
        if not txt:
            continue
        style_name = (p.style.name if p.style else "").lower()
        is_heading = style_name.startswith("heading 1") or style_name.startswith("heading 2") or style_name == "标题 1"
        if is_heading or (len(txt) <= 68 and CHAPTER_TITLE_RE.match(txt)):
            if cur_paras:
                chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
                cur_paras = []
            cur_title = txt
        else:
            cur_paras.append(normalize_paragraph_line(txt))

    if cur_paras:
        chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
    if not chapters:
        chapters.append(Chapter(title="正文", paragraphs=["（空白文档）"]))

    return BookDocument(title=title, author=author, chapters=chapters)


def parse_fb2_file(in_path: Path) -> BookDocument:
    text = read_text_smart(in_path)
    title = clean_book_title_from_stem(in_path.stem)
    author = "佚名"
    if BS4_AVAILABLE:
        soup = BeautifulSoup(text, "html.parser")
        bt = soup.find("book-title")
        if bt and bt.get_text(strip=True):
            title = bt.get_text(strip=True)
        au = soup.find("author")
        if au and au.get_text(" ", strip=True):
            author = au.get_text(" ", strip=True)
        chapters: List[Chapter] = []
        for idx, sec in enumerate(soup.find_all("section"), start=1):
            if sec.find("section"):
                continue
            t_tag = sec.find("title")
            ch_title = t_tag.get_text(" ", strip=True) if t_tag else f"第 {idx} 节"
            if t_tag:
                t_tag.decompose()
            paras = [normalize_paragraph_line(p.get_text(" ", strip=True)) for p in sec.find_all("p")]
            paras = [p for p in paras if p]
            if paras:
                chapters.append(Chapter(title=ch_title, paragraphs=paras))
        if chapters:
            return BookDocument(title=title, author=author, chapters=chapters)
    return parse_with_pymupdf(in_path)


def parse_md_file(in_path: Path) -> BookDocument:
    raw = read_text_smart(in_path)
    title = clean_book_title_from_stem(in_path.stem)
    chapters: List[Chapter] = []
    cur_title = "正文"
    cur_paras: List[str] = []

    for line in raw.splitlines():
        s = line.strip()
        if not s:
            continue
        m_h1 = re.match(r"^#\s+(.+)$", s)
        m_h2 = re.match(r"^#{2,3}\s+(.+)$", s)
        if m_h1 and not chapters and not cur_paras:
            title = m_h1.group(1).strip()
            continue
        if m_h1 or m_h2 or (len(s) <= 68 and CHAPTER_TITLE_RE.match(s)):
            heading_txt = (m_h1.group(1) if m_h1 else (m_h2.group(1) if m_h2 else s)).strip()
            if cur_paras:
                chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
                cur_paras = []
            cur_title = heading_txt
        else:
            cur_paras.append(normalize_paragraph_line(s))

    if cur_paras:
        chapters.append(Chapter(title=cur_title, paragraphs=cur_paras))
    if not chapters:
        chapters = split_text_into_chapters(raw)
    return BookDocument(title=title, author="佚名", chapters=chapters)


def parse_novel_to_document(in_path: Path) -> BookDocument:
    ext = in_path.suffix.lower()
    if ext == ".txt":
        txt = read_text_smart(in_path)
        return BookDocument(
            title=clean_book_title_from_stem(in_path.stem),
            author="佚名",
            chapters=split_text_into_chapters(txt),
        )
    if ext == ".epub":
        return parse_epub_file(in_path)
    if ext in {".mobi", ".azw", ".azw3"}:
        return parse_mobi_or_azw3(in_path)
    if ext == ".pdf":
        return parse_with_pymupdf(in_path)
    if ext == ".docx":
        return parse_docx_file(in_path)
    if ext == ".fb2":
        return parse_fb2_file(in_path)
    if ext == ".md":
        return parse_md_file(in_path)
    if ext in {".html", ".htm"}:
        html_str = read_text_smart(in_path)
        title = clean_book_title_from_stem(in_path.stem)
        if BS4_AVAILABLE:
            soup = BeautifulSoup(html_str, "html.parser")
            if soup.title and soup.title.string:
                title = soup.title.string.strip()
        return BookDocument(
            title=title,
            author="佚名",
            chapters=extract_chapters_from_html_string(html_str, fallback_title="正文"),
        )
    raise ValueError(f"暂不支持读取的小说格式: {ext}")


# ---------------------------------------------------------------------------
# 小说输出写入器（EPUB / TXT / MOBI / PDF / DOCX / HTML / MD / FB2）
# ---------------------------------------------------------------------------
def write_book_to_epub(doc: BookDocument, out_path: Path, auto_indent: bool = True) -> None:
    if not EBOOKLIB_AVAILABLE:
        raise RuntimeError("生成 EPUB 需安装 EbookLib：pip install EbookLib")

    book = epub.EpubBook()
    book.set_identifier(f"urn:uuid:{uuid.uuid4()}")
    book.set_title(doc.title)
    book.set_language("zh")
    book.add_author(doc.author or "佚名")

    indent_css = "2em" if auto_indent else "0"
    style_css = f"""
    body {{
        font-family: "PingFang SC", "Songti SC", "Microsoft YaHei", serif;
        line-height: 1.85;
        margin: 4%;
        color: #1f2328;
    }}
    h1 {{
        font-size: 1.45em;
        margin: 1.2em 0 1em 0;
        padding-bottom: 0.35em;
        border-bottom: 1px solid #e5e7eb;
        text-align: center;
    }}
    p {{
        text-indent: {indent_css};
        margin: 0.55em 0;
        text-align: justify;
    }}
    """
    css_item = epub.EpubItem(
        uid="style_main",
        file_name="style/main.css",
        media_type="text/css",
        content=style_css.encode("utf-8"),
    )
    book.add_item(css_item)

    epub_chapters = []
    for idx, ch in enumerate(doc.chapters, start=1):
        c_item = epub.EpubHtml(
            title=ch.title,
            file_name=f"chap_{idx:04d}.xhtml",
            lang="zh",
        )
        paras_html = "\n".join(f"<p>{html.escape(p)}</p>" for p in ch.paragraphs)
        c_item.content = (
            f"<html><head><title>{html.escape(ch.title)}</title>"
            f'<link rel="stylesheet" type="text/css" href="style/main.css"/></head>'
            f"<body><h1>{html.escape(ch.title)}</h1>\n{paras_html}</body></html>"
        ).encode("utf-8")
        c_item.add_item(css_item)
        book.add_item(c_item)
        epub_chapters.append(c_item)

    book.toc = tuple(epub_chapters)
    book.add_item(epub.EpubNcx())
    book.add_item(epub.EpubNav())
    book.spine = ["nav", *epub_chapters]
    epub.write_epub(str(out_path), book, {})


def write_book_to_txt(
    doc: BookDocument, out_path: Path, txt_encoding: str = "utf-8", auto_indent: bool = True
) -> None:
    indent_prefix = "  " if auto_indent else ""
    blocks = [f"《{doc.title}》", f"作者：{doc.author}", ""]
    for ch in doc.chapters:
        blocks.append(ch.title)
        blocks.append("")
        for p in ch.paragraphs:
            blocks.append(f"{indent_prefix}{p}")
        blocks.append("")
    content = "\n".join(blocks)
    out_path.write_text(content, encoding=txt_encoding, errors="replace")


def encode_mobi_html_boundary_safe(html_str: str) -> bytes:
    """
    构造严格对齐 4096 字节逻辑块的 UTF-8 字节流：
    当多字节字符恰好跨越 4096 边界时自动转为纯 ASCII 数字实体 &#xxxx;，
    确保每一个 4096 字节记录均可独立严格按 UTF-8 解码，完美兼容 Kindle 与各类阅读器。
    """
    buf = bytearray()
    for ch in html_str:
        code = ord(ch)
        if code < 128:
            buf.append(code)
        else:
            utf8_b = ch.encode("utf-8")
            rem = 4096 - (len(buf) % 4096)
            if len(utf8_b) > rem:
                buf.extend(f"&#{code};".encode("ascii"))
            else:
                buf.extend(utf8_b)
    return bytes(buf) if buf else b" "


def write_book_to_mobi_pure_python(doc: BookDocument, out_path: Path, auto_indent: bool = True) -> None:
    """纯 Python 零外部依赖构建标准 PalmDOC / MOBI6 (BOOKMOBI) 电子书"""
    indent_str = "  " if auto_indent else ""
    parts = [
        '<html><head><meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>',
        f"<title>{html.escape(doc.title)}</title></head><body>",
        f"<h1>{html.escape(doc.title)}</h1><p>作者：{html.escape(doc.author)}</p><mbp:pagebreak/>",
    ]
    for ch in doc.chapters:
        parts.append(f"<h2>{html.escape(ch.title)}</h2>")
        for p in ch.paragraphs:
            parts.append(f"<p>{indent_str}{html.escape(p)}</p>")
        parts.append("<mbp:pagebreak/>")
    parts.append("</body></html>")

    raw_html_bytes = encode_mobi_html_boundary_safe("".join(parts))
    text_length = len(raw_html_bytes)
    text_records = [raw_html_bytes[i : i + 4096] for i in range(0, text_length, 4096)]
    num_text_records = len(text_records)

    title_bytes = doc.title.encode("utf-8")
    author_bytes = (doc.author or "佚名").encode("utf-8")
    lang_bytes = b"zh-CN"

    # 构建 EXTH 元数据头
    exth_items = [
        struct.pack(">II", 100, 8 + len(author_bytes)) + author_bytes,
        struct.pack(">II", 503, 8 + len(title_bytes)) + title_bytes,
        struct.pack(">II", 524, 8 + len(lang_bytes)) + lang_bytes,
    ]
    exth_body = b"".join(exth_items)
    exth_unpadded_len = 12 + len(exth_body)
    exth_pad = (4 - (exth_unpadded_len % 4)) % 4
    exth_header = (
        b"EXTH"
        + struct.pack(">II", exth_unpadded_len, len(exth_items))
        + exth_body
        + (b"\x00" * exth_pad)
    )

    # PalmDOC Header (16 bytes)
    palmdoc_header = struct.pack(
        ">HHIHHHH",
        1,                 # 1 = 无压缩标准文本块
        0,
        text_length,
        num_text_records,
        4096,              # 固定逻辑块上限 4096
        0,                 # 无加密
        0,
    )

    full_name_offset = 16 + 232 + len(exth_header)
    flis_rec_num = num_text_records + 1
    fcis_rec_num = num_text_records + 2

    # MOBI Header (232 bytes)
    mobi_hdr = bytearray(232)
    mobi_hdr[0:4] = b"MOBI"
    struct.pack_into(">I", mobi_hdr, 4, 232)
    struct.pack_into(">I", mobi_hdr, 8, 2)                      # 2 = Mobipocket Book
    struct.pack_into(">I", mobi_hdr, 12, 65001)                 # 65001 = UTF-8
    struct.pack_into(">I", mobi_hdr, 16, random.randint(1, 0x7FFFFFFF))
    struct.pack_into(">I", mobi_hdr, 20, 6)                     # version 6
    mobi_hdr[24:64] = b"\xff" * 40
    struct.pack_into(">I", mobi_hdr, 64, flis_rec_num)
    struct.pack_into(">I", mobi_hdr, 68, full_name_offset)
    struct.pack_into(">I", mobi_hdr, 72, len(title_bytes))
    struct.pack_into(">I", mobi_hdr, 76, 0x0804)                # zh-CN locale
    struct.pack_into(">I", mobi_hdr, 88, 6)
    struct.pack_into(">I", mobi_hdr, 92, flis_rec_num)
    struct.pack_into(">I", mobi_hdr, 112, 0x40)                 # EXTH 标志位
    struct.pack_into(">I", mobi_hdr, 148, 0xFFFFFFFF)           # DRM offset
    struct.pack_into(">I", mobi_hdr, 152, 0xFFFFFFFF)           # DRM count
    struct.pack_into(">H", mobi_hdr, 176, 1)                    # first content record
    struct.pack_into(">H", mobi_hdr, 178, num_text_records)     # last content record
    struct.pack_into(">I", mobi_hdr, 180, 1)
    struct.pack_into(">I", mobi_hdr, 184, fcis_rec_num)
    struct.pack_into(">I", mobi_hdr, 188, 1)
    struct.pack_into(">I", mobi_hdr, 192, flis_rec_num)
    struct.pack_into(">I", mobi_hdr, 196, 1)
    struct.pack_into(">I", mobi_hdr, 208, 0xFFFFFFFF)
    mobi_hdr[216:224] = b"\xff" * 8
    struct.pack_into(">I", mobi_hdr, 224, 0)                    # extra_data_flags = 0
    struct.pack_into(">I", mobi_hdr, 228, 0xFFFFFFFF)           # NCX index

    title_pad = (4 - (len(title_bytes) % 4)) % 4 + 2
    rec0 = palmdoc_header + bytes(mobi_hdr) + exth_header + title_bytes + (b"\x00" * title_pad)

    flis_record = (
        b"FLIS\x00\x00\x00\x08\x00\x41\x00\x00\x00\x00\x00\x00"
        b"\xff\xff\xff\xff\x00\x01\x00\x03\x00\x00\x00\x03\x00\x00\x00\x01\xff\xff\xff\xff"
    )
    fcis_record = (
        b"FCIS\x00\x00\x00\x14\x00\x00\x00\x10\x00\x00\x00\x01\x00\x00\x00\x00"
        + struct.pack(">I", text_length)
        + b"\x00\x00\x00\x00\x00\x00\x00\x20\x00\x00\x00\x08\x00\x01\x00\x01\x00\x00\x00\x00"
    )
    eof_record = b"\xe9\x8e\r\n"

    all_records = [rec0, *text_records, flis_record, fcis_record, eof_record]
    total_recs = len(all_records)

    safe_ascii_name = re.sub(r"[^A-Za-z0-9_-]", "_", doc.title)[:31].encode("ascii") or b"Book"
    pdb_name = safe_ascii_name.ljust(32, b"\x00")
    now_ts = int(time.time())
    pdb_header = struct.pack(
        ">32sHHIIIIII4s4sIIH",
        pdb_name,
        0,
        0,
        now_ts,
        now_ts,
        0,
        0,
        0,
        0,
        b"BOOK",
        b"MOBI",
        0,
        0,
        total_recs,
    )

    offset = 78 + total_recs * 8 + 2
    rec_info_list = bytearray()
    for idx, rec in enumerate(all_records):
        uid = idx * 2
        uid_3b = struct.pack(">I", uid)[1:]
        rec_info_list.extend(struct.pack(">IB3s", offset, 0, uid_3b))
        offset += len(rec)

    with open(out_path, "wb") as f:
        f.write(pdb_header)
        f.write(rec_info_list)
        f.write(b"\x00\x00")
        for rec in all_records:
            f.write(rec)


def write_book_to_mobi(doc: BookDocument, out_path: Path, auto_indent: bool = True) -> None:
    calibre_bin = resolve_calibre_convert()
    if calibre_bin and EBOOKLIB_AVAILABLE:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_epub = Path(tmpdir) / "source.epub"
            write_book_to_epub(doc, tmp_epub, auto_indent=auto_indent)
            creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            res = subprocess.run(
                [calibre_bin, str(tmp_epub), str(out_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=creationflags,
            )
            if res.returncode == 0 and out_path.is_file():
                return
    write_book_to_mobi_pure_python(doc, out_path, auto_indent=auto_indent)


def write_book_to_pdf(
    doc: BookDocument,
    out_path: Path,
    page_preset: str = "a4",
    auto_indent: bool = True,
    progress_cb: Optional[Callable[[int], None]] = None,
    is_cancelled: Optional[Callable[[], bool]] = None,
) -> None:
    if not PYMUPDF_AVAILABLE:
        raise RuntimeError("生成 PDF 需安装 PyMuPDF：pip install pymupdf")

    indent_em = "2em" if auto_indent else "0"
    if page_preset == "eink6":
        mediabox = pymupdf.Rect(0, 0, 288, 412)
        margin = 22
        font_pt = 11.5
    elif page_preset == "b5":
        mediabox = pymupdf.paper_rect("b5-jis")
        margin = 38
        font_pt = 11.5
    else:
        mediabox = pymupdf.paper_rect("a4")
        margin = 48
        font_pt = 11.5

    where = mediabox + (margin, margin, -margin, -margin)
    css = f"""
    body {{ font-family: sans-serif; font-size: {font_pt}pt; line-height: 1.75; color: #1c1e21; }}
    h1 {{ font-size: {font_pt + 4}pt; text-align: center; margin-bottom: 14pt; }}
    h2 {{ font-size: {font_pt + 2.5}pt; margin-top: 6pt; margin-bottom: 10pt; }}
    p {{ text-indent: {indent_em}; margin-top: 0; margin-bottom: 6pt; text-align: justify; }}
    """

    tmp_pdf = out_path.parent / f".tmp_{uuid.uuid4().hex}.pdf"
    toc_entries: List[List[object]] = []
    total_ch = max(1, len(doc.chapters))

    with PYMUPDF_LOCK:
        try:
            writer = pymupdf.DocumentWriter(str(tmp_pdf))
            current_page_num = 0

            for idx, ch in enumerate(doc.chapters):
                if is_cancelled and is_cancelled():
                    writer.close()
                    tmp_pdf.unlink(missing_ok=True)
                    raise InterruptedError("用户已中止转换")

                header_html = ""
                if idx == 0:
                    header_html = (
                        f"<h1>{html.escape(doc.title)}</h1>"
                        f"<p style='text-indent:0;text-align:center;color:#666;'>作者：{html.escape(doc.author)}</p>"
                    )
                paras_html = "".join(f"<p>{html.escape(p)}</p>" for p in ch.paragraphs)
                chap_html = f"<html><body>{header_html}<h2>{html.escape(ch.title)}</h2>{paras_html}</body></html>"

                story = pymupdf.Story(html=chap_html, user_css=css)
                toc_entries.append([1, ch.title, current_page_num + 1])

                more = 1
                while more:
                    dev = writer.begin_page(mediabox)
                    more, _ = story.place(where)
                    story.draw(dev)
                    writer.end_page()
                    current_page_num += 1

                if progress_cb:
                    progress_cb(min(94, 20 + int((idx + 1) / total_ch * 74)))

            writer.close()

            # 写入 PDF 书签目录树与元数据
            pdf_doc = pymupdf.open(str(tmp_pdf))
            try:
                pdf_doc.set_toc(toc_entries)
                pdf_doc.set_metadata({"title": doc.title, "author": doc.author, "creator": "OmniConvert"})
                pdf_doc.save(str(out_path), garbage=3, deflate=True)
            finally:
                pdf_doc.close()
        finally:
            tmp_pdf.unlink(missing_ok=True)


def write_book_to_docx(doc: BookDocument, out_path: Path, auto_indent: bool = True) -> None:
    if not DOCX_AVAILABLE:
        raise RuntimeError("生成 DOCX 需安装库：pip install python-docx")

    d = docx.Document()
    d.core_properties.title = doc.title
    d.core_properties.author = doc.author
    d.add_heading(doc.title, level=0)
    d.add_paragraph(f"作者：{doc.author}")

    indent_prefix = "  " if auto_indent else ""
    for ch in doc.chapters:
        d.add_heading(ch.title, level=1)
        for p in ch.paragraphs:
            d.add_paragraph(f"{indent_prefix}{p}")

    d.save(str(out_path))


def write_book_to_html(doc: BookDocument, out_path: Path, auto_indent: bool = True) -> None:
    """生成自带侧边目录、护眼/夜间主题切换与字号调节的单文件离线阅读器"""
    indent_css = "2em" if auto_indent else "0"
    toc_links = []
    chap_blocks = []
    for idx, ch in enumerate(doc.chapters, start=1):
        cid = f"ch-{idx}"
        toc_links.append(f'<a href="#{cid}" class="toc-item">{html.escape(ch.title)}</a>')
        ps = "\n".join(f"<p>{html.escape(p)}</p>" for p in ch.paragraphs)
        chap_blocks.append(f'<section id="{cid}" class="chapter"><h2>{html.escape(ch.title)}</h2>{ps}</section>')

    page_html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>{html.escape(doc.title)}</title>
<style>
:root {{ --bg:#f7f5f0; --card:#ffffff; --fg:#22252a; --muted:#6b7280; --border:#e5e2dc; --accent:#2563eb; --fs:18px; }}
body.sepia {{ --bg:#f4ecd8; --card:#fbf5e6; --fg:#3b2f24; --muted:#7a6855; --border:#e3d5b8; --accent:#9a5b13; }}
body.dark {{ --bg:#121417; --card:#1b1f24; --fg:#d9dee5; --muted:#8b949e; --border:#2d333b; --accent:#58a6ff; }}
* {{ box-sizing: border-box; }}
body {{ margin:0; background:var(--bg); color:var(--fg); font-family:"PingFang SC","Microsoft YaHei",serif; display:flex; min-height:100vh; transition:background .2s,color .2s; }}
aside {{ width:280px; background:var(--card); border-right:1px solid var(--border); height:100vh; position:sticky; top:0; overflow-y:auto; padding:18px 14px; flex-shrink:0; }}
aside.hidden {{ display:none; }}
.book-meta h1 {{ font-size:18px; margin:0 0 6px 0; }}
.book-meta p {{ font-size:13px; color:var(--muted); margin:0 0 14px 0; }}
.toc-item {{ display:block; padding:7px 10px; color:var(--fg); text-decoration:none; border-radius:6px; font-size:14px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
.toc-item:hover {{ background:rgba(128,128,128,0.12); color:var(--accent); }}
main {{ flex:1; padding:24px 20px 80px; max-width:860px; margin:0 auto; }}
.toolbar {{ position:sticky; top:12px; background:var(--card); border:1px solid var(--border); border-radius:10px; padding:8px 14px; display:flex; gap:8px; align-items:center; margin-bottom:28px; box-shadow:0 4px 14px rgba(0,0,0,.05); flex-wrap:wrap; }}
.toolbar button {{ border:1px solid var(--border); background:var(--bg); color:var(--fg); border-radius:6px; padding:5px 11px; cursor:pointer; font-size:13px; }}
.chapter {{ background:var(--card); border:1px solid var(--border); border-radius:12px; padding:32px 36px; margin-bottom:24px; }}
.chapter h2 {{ margin-top:0; padding-bottom:12px; border-bottom:1px solid var(--border); font-size:calc(var(--fs) * 1.3); }}
.chapter p {{ font-size:var(--fs); line-height:1.9; text-indent:{indent_css}; margin:0.65em 0; text-align:justify; }}
@media (max-width: 768px) {{ aside {{ position:fixed; z-index:20; }} .chapter {{ padding:20px 18px; }} }}
</style>
</head>
<body class="sepia">
<aside id="sidebar">
  <div class="book-meta">
    <h1>{html.escape(doc.title)}</h1>
    <p>作者：{html.escape(doc.author)} · 共 {len(doc.chapters)} 章</p>
  </div>
  {"".join(toc_links)}
</aside>
<main>
  <div class="toolbar">
    <button onclick="document.getElementById('sidebar').classList.toggle('hidden')">☰ 目录开关</button>
    <button onclick="document.body.className=''">纯净白</button>
    <button onclick="document.body.className='sepia'">护眼羊皮纸</button>
    <button onclick="document.body.className='dark'">夜间深色</button>
    <button onclick="chgFs(-2)">A-</button>
    <button onclick="chgFs(2)">A+</button>
  </div>
  {"".join(chap_blocks)}
</main>
<script>
let fs = 18;
function chgFs(d) {{ fs = Math.min(30, Math.max(13, fs + d)); document.documentElement.style.setProperty('--fs', fs + 'px'); }}
</script>
</body>
</html>"""
    out_path.write_text(page_html, encoding="utf-8")


def write_book_to_md(doc: BookDocument, out_path: Path) -> None:
    lines = [f"# {doc.title}", "", f"> 作者：{doc.author}", ""]
    for ch in doc.chapters:
        lines.append(f"## {ch.title}")
        lines.append("")
        for p in ch.paragraphs:
            lines.append(p)
            lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def write_book_to_fb2(doc: BookDocument, out_path: Path) -> None:
    sections = []
    for ch in doc.chapters:
        ps = "\n".join(f"      <p>{html.escape(p)}</p>" for p in ch.paragraphs)
        sections.append(
            f"    <section>\n      <title><p>{html.escape(ch.title)}</p></title>\n{ps}\n    </section>"
        )
    fb2_xml = f"""<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
  <description>
    <title-info>
      <genre>prose</genre>
      <author><nickname>{html.escape(doc.author)}</nickname></author>
      <book-title>{html.escape(doc.title)}</book-title>
      <lang>zh</lang>
    </title-info>
  </description>
  <body>
{chr(10).join(sections)}
  </body>
</FictionBook>
"""
    out_path.write_text(fb2_xml, encoding="utf-8")


def convert_novel(
    in_path: Path,
    out_path: Path,
    target_fmt: str,
    auto_indent: bool = True,
    custom_author: str = "",
    txt_encoding: str = "utf-8",
    pdf_preset: str = "a4",
    progress_cb: Optional[Callable[[int], None]] = None,
    is_cancelled: Optional[Callable[[], bool]] = None,
) -> None:
    if progress_cb:
        progress_cb(10)

    doc = parse_novel_to_document(in_path)
    if custom_author.strip():
        doc.author = custom_author.strip()

    if is_cancelled and is_cancelled():
        raise InterruptedError("用户已中止转换")

    if progress_cb:
        progress_cb(45)

    fmt = target_fmt.lower()
    if fmt == "epub":
        write_book_to_epub(doc, out_path, auto_indent=auto_indent)
    elif fmt == "txt":
        write_book_to_txt(doc, out_path, txt_encoding=txt_encoding, auto_indent=auto_indent)
    elif fmt == "mobi":
        write_book_to_mobi(doc, out_path, auto_indent=auto_indent)
    elif fmt == "pdf":
        write_book_to_pdf(
            doc,
            out_path,
            page_preset=pdf_preset,
            auto_indent=auto_indent,
            progress_cb=progress_cb,
            is_cancelled=is_cancelled,
        )
    elif fmt == "docx":
        write_book_to_docx(doc, out_path, auto_indent=auto_indent)
    elif fmt == "html":
        write_book_to_html(doc, out_path, auto_indent=auto_indent)
    elif fmt == "md":
        write_book_to_md(doc, out_path)
    elif fmt == "fb2":
        write_book_to_fb2(doc, out_path)
    else:
        raise ValueError(f"不支持的小说输出格式: {target_fmt}")

    if progress_cb:
        progress_cb(100)


# ===========================================================================
# 后台异步批量转换工作线程
# ===========================================================================
@dataclass
class TaskItem:
    row_id: int
    in_path: Path
    mode: str


class BatchConversionWorker(QThread):
    task_progress = Signal(int, int, str)               # row_id, percent, status_text
    task_finished = Signal(int, bool, str, str, int)    # row_id, ok, out_path_str, message, out_bytes
    batch_finished = Signal(int, int, int)              # success_cnt, fail_cnt, saved_bytes

    def __init__(self, tasks: List[TaskItem], options: Dict[str, object], parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.tasks = tasks
        self.options = options
        self._cancelled = False

    def request_cancel(self) -> None:
        self._cancelled = True

    def is_cancelled(self) -> bool:
        return self._cancelled

    def run(self) -> None:
        success_cnt = 0
        fail_cnt = 0
        total_saved_bytes = 0

        out_dir_str = str(self.options.get("out_dir", "")).strip()
        out_dir = Path(out_dir_str) if out_dir_str else None
        target_fmt = str(self.options["target_fmt"]).lower()
        comp_level = str(self.options.get("comp_level", "balanced"))

        for task in self.tasks:
            if self._cancelled:
                self.task_finished.emit(task.row_id, False, "", "已取消", 0)
                continue

            in_path = task.in_path
            if not in_path.is_file():
                fail_cnt += 1
                self.task_finished.emit(task.row_id, False, "", "源文件不存在", 0)
                continue

            in_size = in_path.stat().st_size
            suffix_tag = f"_{comp_level}" if task.mode != MODE_NOVEL else "_converted"
            out_path = resolve_safe_output_path(in_path, out_dir, target_fmt, suffix_hint=suffix_tag)

            def on_prog(pct: int, r: int = task.row_id) -> None:
                self.task_progress.emit(r, pct, f"处理中 {pct}%")

            self.task_progress.emit(task.row_id, 2, "准备中...")

            try:
                if task.mode == MODE_IMAGE:
                    convert_image(
                        in_path=in_path,
                        out_path=out_path,
                        target_fmt=target_fmt,
                        comp_level=comp_level,
                        max_dim=int(self.options.get("img_max_dim", 0)),
                        strip_exif=bool(self.options.get("img_strip_exif", False)),
                        progress_cb=on_prog,
                    )
                elif task.mode == MODE_AUDIO:
                    convert_audio(
                        in_path=in_path,
                        out_path=out_path,
                        target_fmt=target_fmt,
                        comp_level=comp_level,
                        sample_rate=int(self.options.get("aud_sample_rate", 0)),
                        loudnorm=bool(self.options.get("aud_loudnorm", False)),
                        progress_cb=on_prog,
                        is_cancelled=self.is_cancelled,
                    )
                elif task.mode == MODE_VIDEO:
                    convert_video(
                        in_path=in_path,
                        out_path=out_path,
                        target_fmt=target_fmt,
                        comp_level=comp_level,
                        max_width=int(self.options.get("vid_max_width", 0)),
                        codec_pref=str(self.options.get("vid_codec", "h264")),
                        mute_audio=bool(self.options.get("vid_mute", False)),
                        progress_cb=on_prog,
                        is_cancelled=self.is_cancelled,
                    )
                elif task.mode == MODE_NOVEL:
                    convert_novel(
                        in_path=in_path,
                        out_path=out_path,
                        target_fmt=target_fmt,
                        auto_indent=bool(self.options.get("nov_indent", True)),
                        custom_author=str(self.options.get("nov_author", "")),
                        txt_encoding=str(self.options.get("nov_txt_enc", "utf-8")),
                        pdf_preset=str(self.options.get("nov_pdf_preset", "a4")),
                        progress_cb=on_prog,
                        is_cancelled=self.is_cancelled,
                    )

                out_size = out_path.stat().st_size if out_path.is_file() else 0
                if in_size > out_size > 0:
                    total_saved_bytes += in_size - out_size
                success_cnt += 1
                self.task_finished.emit(task.row_id, True, str(out_path), "✅ 完成 (双击定位)", out_size)

            except InterruptedError:
                out_path.unlink(missing_ok=True)
                self.task_finished.emit(task.row_id, False, "", "⏹ 已中止", 0)
            except Exception as exc:
                out_path.unlink(missing_ok=True)
                fail_cnt += 1
                self.task_finished.emit(task.row_id, False, "", f"❌ 失败: {exc}", 0)

        self.batch_finished.emit(success_cnt, fail_cnt, total_saved_bytes)


# ===========================================================================
# 现代双主题样式表（深色 & 浅色）
# ===========================================================================
def build_stylesheet(dark: bool) -> str:
    if dark:
        bg_win = "#0f1117"
        bg_side = "#161922"
        bg_card = "#1c202b"
        bg_input = "#242938"
        fg_main = "#e6edf3"
        fg_sub = "#8b949e"
        border = "#2d3446"
        accent = "#3b82f6"
        accent_hover = "#2563eb"
        drop_bg = "#182032"
        drop_border = "#3b82f6"
        row_alt = "#181c26"
    else:
        bg_win = "#f3f5f9"
        bg_side = "#ffffff"
        bg_card = "#ffffff"
        bg_input = "#f8fafc"
        fg_main = "#1e293b"
        fg_sub = "#64748b"
        border = "#e2e8f0"
        accent = "#2563eb"
        accent_hover = "#1d4ed8"
        drop_bg = "#eff6ff"
        drop_border = "#3b82f6"
        row_alt = "#f8fafc"

    return f"""
    QMainWindow, QWidget {{
        background-color: {bg_win};
        color: {fg_main};
        font-family: "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei UI", "Microsoft YaHei", system-ui, sans-serif;
        font-size: 13px;
    }}
    QFrame#Sidebar {{
        background-color: {bg_side};
        border-right: 1px solid {border};
    }}
    QFrame#Card {{
        background-color: {bg_card};
        border: 1px solid {border};
        border-radius: 12px;
    }}
    QFrame#DropZone {{
        background-color: {drop_bg};
        border: 2px dashed {drop_border};
        border-radius: 12px;
    }}
    QLabel {{
        background: transparent;
    }}
    QLabel#AppTitle {{
        font-size: 18px;
        font-weight: 700;
    }}
    QLabel#Muted {{
        color: {fg_sub};
        font-size: 12px;
    }}
    QLabel#SectionTitle {{
        font-size: 14px;
        font-weight: 600;
    }}
    QPushButton {{
        background-color: {bg_input};
        color: {fg_main};
        border: 1px solid {border};
        border-radius: 8px;
        padding: 7px 14px;
        font-weight: 500;
    }}
    QPushButton:hover {{
        border-color: {accent};
    }}
    QPushButton:disabled {{
        color: {fg_sub};
    }}
    QPushButton#NavBtn {{
        text-align: left;
        padding: 11px 14px;
        border-radius: 9px;
        border: 1px solid transparent;
        background: transparent;
        font-size: 13px;
    }}
    QPushButton#NavBtn:hover {{
        background-color: {bg_input};
    }}
    QPushButton#NavBtn:checked {{
        background-color: {accent};
        color: #ffffff;
        font-weight: 600;
    }}
    QPushButton#PrimaryBtn {{
        background-color: {accent};
        color: #ffffff;
        border: none;
        border-radius: 9px;
        padding: 10px 24px;
        font-size: 14px;
        font-weight: 600;
    }}
    QPushButton#PrimaryBtn:hover {{
        background-color: {accent_hover};
    }}
    QPushButton#StopBtn {{
        background-color: #ef4444;
        color: #ffffff;
        border: none;
        border-radius: 9px;
        padding: 10px 24px;
        font-size: 14px;
        font-weight: 600;
    }}
    QComboBox, QLineEdit {{
        background-color: {bg_input};
        color: {fg_main};
        border: 1px solid {border};
        border-radius: 7px;
        padding: 6px 10px;
        min-height: 20px;
    }}
    QComboBox:focus, QLineEdit:focus {{
        border-color: {accent};
    }}
    QTableWidget {{
        background-color: {bg_card};
        alternate-background-color: {row_alt};
        border: 1px solid {border};
        border-radius: 10px;
        gridline-color: transparent;
        selection-background-color: {accent};
        selection-color: #ffffff;
    }}
    QHeaderView::section {{
        background-color: {bg_input};
        color: {fg_sub};
        border: none;
        border-bottom: 1px solid {border};
        padding: 8px 10px;
        font-weight: 600;
    }}
    QProgressBar {{
        background-color: {bg_input};
        border: 1px solid {border};
        border-radius: 6px;
        text-align: center;
        height: 16px;
        font-size: 11px;
    }}
    QProgressBar::chunk {{
        background-color: {accent};
        border-radius: 5px;
    }}
    QCheckBox {{
        background: transparent;
        spacing: 6px;
    }}
    """


# ===========================================================================
# 自定义拖拽区域控件
# ===========================================================================
class DropZoneFrame(QFrame):
    paths_dropped = Signal(list)

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setObjectName("DropZone")
        self.setAcceptDrops(True)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:
        paths = []
        for url in event.mimeData().urls():
            local = url.toLocalFile()
            if local:
                paths.append(Path(local))
        if paths:
            self.paths_dropped.emit(paths)
        event.acceptProposedAction()


# ===========================================================================
# 主界面窗口
# ===========================================================================
class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("OmniConvert 万能格式工坊 · 离线便携跨平台版")
        self.resize(1100, 740)
        self.setMinimumSize(920, 620)
        self.setAcceptDrops(True)

        self.config_path = get_portable_config_path()
        self.settings = self._load_settings()
        self.dark_mode: bool = bool(self.settings.get("dark_mode", True))
        self.current_mode: str = MODE_IMAGE
        self.worker: Optional[BatchConversionWorker] = None

        # 存储每一行的任务元数据：row -> dict
        self.file_records: List[Dict[str, object]] = []

        self._build_ui()
        self._apply_theme()
        self._restore_saved_ui_state()
        self._refresh_engine_status()

    # -----------------------------------------------------------------------
    # 便携配置存取
    # -----------------------------------------------------------------------
    def _load_settings(self) -> Dict[str, object]:
        if self.config_path.is_file():
            try:
                return json.loads(self.config_path.read_text(encoding="utf-8"))
            except Exception:
                return {}
        return {}

    def _save_settings(self) -> None:
        data = {
            "dark_mode": self.dark_mode,
            "last_mode": self.current_mode,
            "out_dir": self.out_dir_edit.text().strip(),
            "use_custom_dir": self.out_mode_combo.currentIndex() == 1,
        }
        try:
            self.config_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception:
            pass

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self._save_settings()
        if self.worker and self.worker.isRunning():
            self.worker.request_cancel()
            self.worker.wait(2000)
        super().closeEvent(event)

    # -----------------------------------------------------------------------
    # 构建界面布局
    # -----------------------------------------------------------------------
    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root_layout = QHBoxLayout(central)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        # 1. 左侧导航栏
        sidebar = QFrame()
        sidebar.setObjectName("Sidebar")
        sidebar.setFixedWidth(230)
        side_layout = QVBoxLayout(sidebar)
        side_layout.setContentsMargins(16, 22, 16, 18)
        side_layout.setSpacing(8)

        title_lbl = QLabel("⚡ OmniConvert")
        title_lbl.setObjectName("AppTitle")
        sub_lbl = QLabel("离线便携 · 万能转换压缩工坊")
        sub_lbl.setObjectName("Muted")
        side_layout.addWidget(title_lbl)
        side_layout.addWidget(sub_lbl)
        side_layout.addSpacing(14)

        self.nav_group = QButtonGroup(self)
        self.nav_group.setExclusive(True)
        self.nav_buttons: Dict[str, QPushButton] = {}

        nav_defs = [
            (MODE_IMAGE, "🖼️  图片压缩与转换"),
            (MODE_AUDIO, "🎵  音频转换与提音"),
            (MODE_VIDEO, "🎬  视频压缩与动图"),
            (MODE_NOVEL, "📚  小说格式互转"),
        ]
        for idx, (mode_key, text) in enumerate(nav_defs):
            btn = QPushButton(text)
            btn.setObjectName("NavBtn")
            btn.setCheckable(True)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            if idx == 0:
                btn.setChecked(True)
            btn.clicked.connect(lambda checked, m=mode_key: self._switch_mode(m))
            self.nav_group.addButton(btn)
            self.nav_buttons[mode_key] = btn
            side_layout.addWidget(btn)

        side_layout.addStretch(1)

        # 引擎健康指示
        self.engine_status_btn = QPushButton("🟢 离线引擎状态")
        self.engine_status_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.engine_status_btn.clicked.connect(self._show_engine_report_dialog)
        side_layout.addWidget(self.engine_status_btn)

        self.theme_btn = QPushButton("☀️ 切换浅色模式")
        self.theme_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.theme_btn.clicked.connect(self._toggle_theme)
        side_layout.addWidget(self.theme_btn)

        root_layout.addWidget(sidebar)

        # 2. 右侧主工作区
        workspace = QWidget()
        ws_layout = QVBoxLayout(workspace)
        ws_layout.setContentsMargins(20, 18, 20, 18)
        ws_layout.setSpacing(12)

        # 顶部拖拽区与工具按钮栏
        self.drop_zone = DropZoneFrame()
        self.drop_zone.paths_dropped.connect(self._handle_dropped_paths)
        dz_layout = QHBoxLayout(self.drop_zone)
        dz_layout.setContentsMargins(18, 12, 18, 12)

        dz_text_box = QVBoxLayout()
        self.mode_headline_lbl = QLabel("图片多级压缩与全格式转换")
        self.mode_headline_lbl.setObjectName("SectionTitle")
        self.mode_hint_lbl = QLabel("拖入文件或整个文件夹到窗口任意位置 · 支持 JPG / PNG / WEBP / AVIF / HEIC / ICO 等")
        self.mode_hint_lbl.setObjectName("Muted")
        dz_text_box.addWidget(self.mode_headline_lbl)
        dz_text_box.addWidget(self.mode_hint_lbl)
        dz_layout.addLayout(dz_text_box, 1)

        btn_add_files = QPushButton("➕ 添加文件")
        btn_add_files.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_add_files.clicked.connect(self._browse_add_files)

        btn_add_dir = QPushButton("📁 添加文件夹")
        btn_add_dir.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_add_dir.clicked.connect(self._browse_add_folder)

        btn_clear_done = QPushButton("🧹 清理已完成")
        btn_clear_done.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_clear_done.clicked.connect(self._clear_completed_rows)

        btn_clear_all = QPushButton("🗑️ 清空列表")
        btn_clear_all.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_clear_all.clicked.connect(self._clear_all_rows)

        for b in (btn_add_files, btn_add_dir, btn_clear_done, btn_clear_all):
            dz_layout.addWidget(b)

        ws_layout.addWidget(self.drop_zone)

        # 中部任务列表表格
        self.table = QTableWidget(0, 5)
        self.table.setHorizontalHeaderLabels(["文件名", "原格式 · 原始体积", "输出体积 · 节省比", "进度", "状态 (双击打开位置)"])
        self.table.setAlternatingRowColors(True)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.verticalHeader().setVisible(False)
        self.table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.table.customContextMenuRequested.connect(self._show_table_context_menu)
        self.table.cellDoubleClicked.connect(self._on_row_double_clicked)

        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(3, 130)
        header.setSectionResizeMode(4, QHeaderView.ResizeMode.ResizeToContents)

        # Delete / Backspace 快捷键移除选中行
        del_action = QAction(self)
        del_action.setShortcuts([QKeySequence.StandardKey.Delete, QKeySequence(Qt.Key.Key_Backspace)])
        del_action.triggered.connect(self._remove_selected_rows)
        self.table.addAction(del_action)

        ws_layout.addWidget(self.table, 1)

        # 参数配置卡片（随分类切换联动）
        settings_card = QFrame()
        settings_card.setObjectName("Card")
        sc_layout = QVBoxLayout(settings_card)
        sc_layout.setContentsMargins(16, 12, 16, 12)
        sc_layout.setSpacing(10)

        top_opts_row = QHBoxLayout()
        top_opts_row.addWidget(QLabel("目标格式:"))
        self.target_fmt_combo = QComboBox()
        self.target_fmt_combo.setMinimumWidth(115)
        top_opts_row.addWidget(self.target_fmt_combo)

        top_opts_row.addSpacing(14)
        self.comp_label = QLabel("压缩强度:")
        top_opts_row.addWidget(self.comp_label)
        self.comp_combo = QComboBox()
        for key, desc in COMPRESSION_PRESETS:
            self.comp_combo.addItem(desc, key)
        self.comp_combo.setCurrentIndex(2)  # 默认均衡压缩
        self.comp_combo.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        top_opts_row.addWidget(self.comp_combo, 1)
        sc_layout.addLayout(top_opts_row)

        # 专属人性化子面板
        self.sub_stack = QStackedWidget()
        self.sub_stack.addWidget(self._build_image_subpanel())
        self.sub_stack.addWidget(self._build_audio_subpanel())
        self.sub_stack.addWidget(self._build_video_subpanel())
        self.sub_stack.addWidget(self._build_novel_subpanel())
        sc_layout.addWidget(self.sub_stack)

        ws_layout.addWidget(settings_card)

        # 底部输出路径与执行控制卡片
        footer_card = QFrame()
        footer_card.setObjectName("Card")
        ft_layout = QVBoxLayout(footer_card)
        ft_layout.setContentsMargins(16, 12, 16, 12)
        ft_layout.setSpacing(10)

        out_row = QHBoxLayout()
        out_row.addWidget(QLabel("输出位置:"))
        self.out_mode_combo = QComboBox()
        self.out_mode_combo.addItems(["保存在源文件所在目录（自动重命名防覆盖）", "统一保存到指定目录"])
        self.out_mode_combo.currentIndexChanged.connect(self._on_out_mode_changed)
        out_row.addWidget(self.out_mode_combo)

        self.out_dir_edit = QLineEdit()
        self.out_dir_edit.setPlaceholderText("请选择输出目录...")
        self.out_dir_edit.setEnabled(False)
        out_row.addWidget(self.out_dir_edit, 1)

        self.btn_pick_dir = QPushButton("浏览...")
        self.btn_pick_dir.setEnabled(False)
        self.btn_pick_dir.clicked.connect(self._pick_output_dir)
        out_row.addWidget(self.btn_pick_dir)

        btn_open_out = QPushButton("📂 打开输出目录")
        btn_open_out.clicked.connect(self._open_current_output_dir)
        out_row.addWidget(btn_open_out)
        ft_layout.addLayout(out_row)

        run_row = QHBoxLayout()
        self.summary_lbl = QLabel("就绪 · 请拖入或添加需要处理的文件")
        self.summary_lbl.setObjectName("Muted")
        run_row.addWidget(self.summary_lbl, 1)

        self.batch_bar = QProgressBar()
        self.batch_bar.setFixedWidth(200)
        self.batch_bar.setValue(0)
        run_row.addWidget(self.batch_bar)

        self.start_btn = QPushButton("🚀 开始批量转换")
        self.start_btn.setObjectName("PrimaryBtn")
        self.start_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.start_btn.clicked.connect(self._on_start_or_stop_clicked)
        run_row.addWidget(self.start_btn)

        ft_layout.addLayout(run_row)
        ws_layout.addWidget(footer_card)

        root_layout.addWidget(workspace, 1)
        self._populate_mode_formats(MODE_IMAGE)

    def _build_image_subpanel(self) -> QWidget:
        w = QWidget()
        ly = QHBoxLayout(w)
        ly.setContentsMargins(0, 0, 0, 0)
        ly.addWidget(QLabel("尺寸限制:"))
        self.img_dim_combo = QComboBox()
        for label, val in [
            ("保持原图分辨率", 0),
            ("限制长边 3840 px (4K)", 3840),
            ("限制长边 2560 px (2K)", 2560),
            ("限制长边 1920 px (1080P)", 1920),
            ("限制长边 1280 px (网页高清)", 1280),
            ("限制长边 800 px (轻量缩略图)", 800),
        ]:
            self.img_dim_combo.addItem(label, val)
        ly.addWidget(self.img_dim_combo)
        ly.addSpacing(16)
        self.img_strip_exif_chk = QCheckBox("清除 EXIF 拍摄参数与地理位置隐私（自动保留正确旋转方向）")
        ly.addWidget(self.img_strip_exif_chk)
        ly.addStretch(1)
        return w

    def _build_audio_subpanel(self) -> QWidget:
        w = QWidget()
        ly = QHBoxLayout(w)
        ly.setContentsMargins(0, 0, 0, 0)
        ly.addWidget(QLabel("采样率:"))
        self.aud_sr_combo = QComboBox()
        for label, val in [
            ("保持原音频采样率", 0),
            ("48000 Hz (影视级)", 48000),
            ("44100 Hz (CD 标准)", 44100),
            ("24000 Hz (语音有声书)", 24000),
        ]:
            self.aud_sr_combo.addItem(label, val)
        ly.addWidget(self.aud_sr_combo)
        ly.addSpacing(16)
        self.aud_loudnorm_chk = QCheckBox("开启智能响度均衡 (自动统一录音/播客音量大小)")
        ly.addWidget(self.aud_loudnorm_chk)
        ly.addStretch(1)
        return w

    def _build_video_subpanel(self) -> QWidget:
        w = QWidget()
        ly = QHBoxLayout(w)
        ly.setContentsMargins(0, 0, 0, 0)
        ly.addWidget(QLabel("画面尺寸:"))
        self.vid_res_combo = QComboBox()
        for label, val in [
            ("保持原分辨率", 0),
            ("最高 1080P (1920宽)", 1920),
            ("最高 720P (1280宽)", 1280),
            ("最高 480P (854宽)", 854),
        ]:
            self.vid_res_combo.addItem(label, val)
        ly.addWidget(self.vid_res_combo)

        ly.addSpacing(12)
        ly.addWidget(QLabel("编码器:"))
        self.vid_codec_combo = QComboBox()
        self.vid_codec_combo.addItem("H.264 (全设备兼容性最佳)", "h264")
        self.vid_codec_combo.addItem("H.265 / HEVC (同画质体积更小)", "h265")
        ly.addWidget(self.vid_codec_combo)

        ly.addSpacing(12)
        self.vid_mute_chk = QCheckBox("移除音轨（导出静音纯画面视频）")
        ly.addWidget(self.vid_mute_chk)
        ly.addStretch(1)
        return w

    def _build_novel_subpanel(self) -> QWidget:
        w = QWidget()
        ly = QHBoxLayout(w)
        ly.setContentsMargins(0, 0, 0, 0)

        self.nov_indent_chk = QCheckBox("自动段首缩进两格并清理空行")
        self.nov_indent_chk.setChecked(True)
        ly.addWidget(self.nov_indent_chk)

        ly.addSpacing(10)
        ly.addWidget(QLabel("PDF版式:"))
        self.nov_pdf_combo = QComboBox()
        self.nov_pdf_combo.addItem("A4 标准版", "a4")
        self.nov_pdf_combo.addItem("B5 舒适阅读", "b5")
        self.nov_pdf_combo.addItem("6寸 电纸书/手机大字版", "eink6")
        ly.addWidget(self.nov_pdf_combo)

        ly.addSpacing(10)
        ly.addWidget(QLabel("TXT编码:"))
        self.nov_enc_combo = QComboBox()
        self.nov_enc_combo.addItem("UTF-8 (现代通用推荐)", "utf-8")
        self.nov_enc_combo.addItem("GB18030 (兼容老式MP3/车载阅读)", "gb18030")
        ly.addWidget(self.nov_enc_combo)

        ly.addSpacing(10)
        ly.addWidget(QLabel("作者署名:"))
        self.nov_author_edit = QLineEdit()
        self.nov_author_edit.setPlaceholderText("留空则自动识别原书作者")
        self.nov_author_edit.setMaximumWidth(160)
        ly.addWidget(self.nov_author_edit)
        ly.addStretch(1)
        return w

    # -----------------------------------------------------------------------
    # 分类与主题联动
    # -----------------------------------------------------------------------
    def _apply_theme(self) -> None:
        self.setStyleSheet(build_stylesheet(self.dark_mode))
        self.theme_btn.setText("☀️ 切换浅色模式" if self.dark_mode else "🌙 切换深色模式")

    def _toggle_theme(self) -> None:
        self.dark_mode = not self.dark_mode
        self._apply_theme()
        self._save_settings()

    def _restore_saved_ui_state(self) -> None:
        saved_dir = str(self.settings.get("out_dir", ""))
        if saved_dir:
            self.out_dir_edit.setText(saved_dir)
        if self.settings.get("use_custom_dir"):
            self.out_mode_combo.setCurrentIndex(1)
        saved_mode = str(self.settings.get("last_mode", MODE_IMAGE))
        if saved_mode in self.nav_buttons:
            self.nav_buttons[saved_mode].setChecked(True)
            self._switch_mode(saved_mode)

    def _switch_mode(self, mode: str) -> None:
        if self.worker and self.worker.isRunning():
            self.nav_buttons[self.current_mode].setChecked(True)
            QMessageBox.information(self, "任务进行中", "请等待当前批量转换完成或点击停止后再切换工坊模块。")
            return

        if mode == self.current_mode and self.target_fmt_combo.count() > 0:
            return

        if self.file_records:
            # 过滤不兼容当前模式的文件，若存在未匹配项温和提示清空
            incompatible = [
                r for r in self.file_records if not is_file_accepted_in_mode(r["in_path"], mode)  # type: ignore[arg-type]
            ]
            if incompatible:
                self._clear_all_rows()

        self.current_mode = mode
        self._populate_mode_formats(mode)

    def _populate_mode_formats(self, mode: str) -> None:
        self.target_fmt_combo.clear()
        if mode == MODE_IMAGE:
            self.mode_headline_lbl.setText("🖼️ 图片多级压缩与全格式转换")
            self.mode_hint_lbl.setText("拖入图片或文件夹 · 支持 JPG / PNG / WEBP / AVIF / HEIC / BMP / TIFF / GIF / ICO")
            fmts = ["WEBP", "JPG", "PNG"]
            if AVIF_AVAILABLE:
                fmts.append("AVIF")
            if HEIF_AVAILABLE:
                fmts.append("HEIC")
            fmts.extend(["ICO", "BMP", "TIFF", "GIF"])
            self.target_fmt_combo.addItems(fmts)
            self.comp_label.setVisible(True)
            self.comp_combo.setVisible(True)
            self.sub_stack.setCurrentIndex(0)

        elif mode == MODE_AUDIO:
            self.mode_headline_lbl.setText("🎵 音频多级压缩转换 & 视频一键提取音频")
            self.mode_hint_lbl.setText("支持直接拖入 MP4/MKV/MOV 视频提取音乐，或 MP3 / FLAC / WAV / M4A / OGG / OPUS 互转")
            self.target_fmt_combo.addItems(["MP3", "M4A", "FLAC", "WAV", "OGG", "OPUS"])
            self.comp_label.setVisible(True)
            self.comp_combo.setVisible(True)
            self.sub_stack.setCurrentIndex(1)

        elif mode == MODE_VIDEO:
            self.mode_headline_lbl.setText("🎬 视频多级瘦身压缩 & 高清 GIF 动图转换")
            self.mode_hint_lbl.setText("支持 MP4 / MKV / MOV / WEBM / AVI / FLV 等多级画质体积压缩与调色盘 GIF 导出")
            self.target_fmt_combo.addItems(["MP4", "MKV", "MOV", "WEBM", "GIF", "AVI"])
            self.comp_label.setVisible(True)
            self.comp_combo.setVisible(True)
            self.sub_stack.setCurrentIndex(2)

        elif mode == MODE_NOVEL:
            self.mode_headline_lbl.setText("📚 小说电子书全格式互转与智能分章排版")
            self.mode_hint_lbl.setText(
                "自动识别 TXT 乱码编码与中英文章节目录 · 支持 TXT / EPUB / MOBI / AZW3 / PDF / DOCX / HTML / MD / FB2"
            )
            self.target_fmt_combo.addItems(["EPUB", "TXT", "MOBI", "PDF", "HTML", "DOCX", "MD", "FB2"])
            self.comp_label.setVisible(False)
            self.comp_combo.setVisible(False)
            self.sub_stack.setCurrentIndex(3)

    # -----------------------------------------------------------------------
    # 引擎状态自检
    # -----------------------------------------------------------------------
    def _refresh_engine_status(self) -> None:
        ffmpeg_ok = resolve_ffmpeg() is not None
        core_ok = PIL_AVAILABLE and ffmpeg_ok and PYMUPDF_AVAILABLE and EBOOKLIB_AVAILABLE
        if core_ok:
            self.engine_status_btn.setText("🟢 离线引擎全部就绪")
        else:
            self.engine_status_btn.setText("🟡 部分可选引擎待安装")

    def _show_engine_report_dialog(self) -> None:
        ff_path = resolve_ffmpeg() or "未检测到 (请 pip install imageio-ffmpeg)"
        calibre_path = resolve_calibre_convert() or "未安装 (已启用内置纯 Python 离线 MOBI 引擎)"
        lines = [
            f"• 图片核心引擎 (Pillow): {'✅ 已就绪' if PIL_AVAILABLE else '❌ 未安装'}",
            f"• AVIF 新一代图像支持: {'✅ 已就绪' if AVIF_AVAILABLE else '⚠️ 当前 Pillow 未包含'}",
            f"• 苹果 HEIC 图像支持 (pillow-heif): {'✅ 已就绪' if HEIF_AVAILABLE else '⚠️ 未安装'}",
            f"• 离线音视频内核 (FFmpeg): {'✅ ' + ff_path if resolve_ffmpeg() else '❌ ' + ff_path}",
            f"• PDF 与电子书渲染引擎 (PyMuPDF): {'✅ 已就绪' if PYMUPDF_AVAILABLE else '❌ 未安装'}",
            f"• EPUB 标准引擎 (EbookLib): {'✅ 已就绪' if EBOOKLIB_AVAILABLE else '❌ 未安装'}",
            f"• Word 文档引擎 (python-docx): {'✅ 已就绪' if DOCX_AVAILABLE else '⚠️ 未安装'}",
            f"• MOBI/AZW3 解包引擎 (mobi): {'✅ 已就绪' if MOBI_UNPACK_AVAILABLE else '⚠️ 使用 PyMuPDF 备用读取'}",
            f"• Calibre 增强插件 (可选): {calibre_path}",
            f"\n便携配置文件位置:\n{self.config_path}",
        ]
        QMessageBox.information(self, "离线引擎健康自检", "\n".join(lines))

    # -----------------------------------------------------------------------
    # 文件导入与全局拖拽智能路由
    # -----------------------------------------------------------------------
    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:
        paths = [Path(u.toLocalFile()) for u in event.mimeData().urls() if u.toLocalFile()]
        if paths:
            self._handle_dropped_paths(paths)
        event.acceptProposedAction()

    def _collect_files_from_paths(self, raw_paths: List[Path]) -> List[Path]:
        collected: List[Path] = []
        for p in raw_paths:
            if p.is_file():
                collected.append(p)
            elif p.is_dir():
                for child in sorted(p.rglob("*")):
                    if child.is_file() and not child.name.startswith("."):
                        collected.append(child)
        return collected

    def _handle_dropped_paths(self, raw_paths: List[Path]) -> None:
        if self.worker and self.worker.isRunning():
            return
        all_files = self._collect_files_from_paths(raw_paths)
        if not all_files:
            return

        accepted = [f for f in all_files if is_file_accepted_in_mode(f, self.current_mode)]

        # 人性化设计：若当前列表为空且用户拖入的是其他分类文件（如在图片页拖入了 .epub 小说），自动帮用户切换分类并导入！
        if not accepted and not self.file_records:
            detected_modes = [detect_category_for_file(f) for f in all_files]
            detected_modes = [m for m in detected_modes if m is not None]
            if detected_modes:
                best_mode = max(set(detected_modes), key=detected_modes.count)
                self.nav_buttons[best_mode].setChecked(True)
                self._switch_mode(best_mode)
                accepted = [f for f in all_files if is_file_accepted_in_mode(f, self.current_mode)]
                mode_names = {
                    MODE_IMAGE: "图片压缩与转换",
                    MODE_AUDIO: "音频转换与提音",
                    MODE_VIDEO: "视频压缩与动图",
                    MODE_NOVEL: "小说格式互转",
                }
                self.summary_lbl.setText(f"✨ 已智能识别文件类型并切换至【{mode_names[best_mode]}】")

        if not accepted:
            QMessageBox.warning(
                self,
                "未匹配到支持的文件",
                "所选文件不属于当前工坊支持的格式，请检查文件后缀或切换到对应分类模块。",
            )
            return

        added = self._append_files_to_table(accepted)
        self.summary_lbl.setText(f"已导入 {added} 个文件 · 当前队列共 {len(self.file_records)} 个任务")

    def _browse_add_files(self) -> None:
        ext_map = {
            MODE_IMAGE: "图片文件 (*.jpg *.jpeg *.png *.webp *.avif *.heic *.heif *.bmp *.tiff *.tif *.gif *.ico)",
            MODE_AUDIO: "音频与视频文件 (*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus *.wma *.mp4 *.mkv *.mov *.avi *.webm)",
            MODE_VIDEO: "视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.flv *.wmv *.m4v *.ts *.mpg *.3gp)",
            MODE_NOVEL: "小说与文档 (*.txt *.epub *.mobi *.azw *.azw3 *.pdf *.docx *.fb2 *.md *.html *.htm)",
        }
        files, _ = QFileDialog.getOpenFileNames(self, "选择需要转换的文件", "", ext_map[self.current_mode])
        if files:
            self._handle_dropped_paths([Path(f) for f in files])

    def _browse_add_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "选择包含待处理文件的文件夹")
        if folder:
            self._handle_dropped_paths([Path(folder)])

    def _append_files_to_table(self, files: List[Path]) -> int:
        existing = {Path(str(r["in_path"])).resolve() for r in self.file_records}
        added = 0
        for f in files:
            resolved = f.resolve()
            if resolved in existing:
                continue
            existing.add(resolved)
            size_b = f.stat().st_size
            row = self.table.rowCount()
            self.table.insertRow(row)

            name_item = QTableWidgetItem(f.name)
            name_item.setToolTip(str(f))

            ext_label = f.suffix.upper().lstrip(".") or "FILE"
            orig_item = QTableWidgetItem(f"{ext_label} · {format_size(size_b)}")
            orig_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)

            after_item = QTableWidgetItem("—")
            after_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)

            prog_bar = QProgressBar()
            prog_bar.setRange(0, 100)
            prog_bar.setValue(0)
            self.table.setCellWidget(row, 3, prog_bar)

            status_item = QTableWidgetItem("⏳ 待处理")
            status_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)

            self.table.setItem(row, 0, name_item)
            self.table.setItem(row, 1, orig_item)
            self.table.setItem(row, 2, after_item)
            self.table.setItem(row, 4, status_item)

            self.file_records.append(
                {
                    "in_path": f,
                    "in_size": size_b,
                    "out_path": "",
                    "done": False,
                }
            )
            added += 1
        return added

    # -----------------------------------------------------------------------
    # 列表管理与右键菜单
    # -----------------------------------------------------------------------
    def _show_table_context_menu(self, pos) -> None:
        menu = QMenu(self)
        act_reveal = menu.addAction("📂 在文件夹中显示")
        act_remove = menu.addAction("🗑️ 移除选中项")
        chosen = menu.exec(self.table.viewport().mapToGlobal(pos))
        if chosen == act_remove:
            self._remove_selected_rows()
        elif chosen == act_reveal:
            rows = sorted({idx.row() for idx in self.table.selectedIndexes()})
            if rows:
                self._on_row_double_clicked(rows[0], 0)

    def _on_row_double_clicked(self, row: int, _col: int) -> None:
        if 0 <= row < len(self.file_records):
            rec = self.file_records[row]
            out_p = str(rec.get("out_path", ""))
            target = Path(out_p) if out_p and Path(out_p).exists() else Path(str(rec["in_path"]))
            reveal_in_file_manager(target)

    def _remove_selected_rows(self) -> None:
        if self.worker and self.worker.isRunning():
            return
        rows = sorted({idx.row() for idx in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)
            self.file_records.pop(r)
        self.summary_lbl.setText(f"当前队列剩余 {len(self.file_records)} 个任务")

    def _clear_completed_rows(self) -> None:
        if self.worker and self.worker.isRunning():
            return
        for r in range(len(self.file_records) - 1, -1, -1):
            if self.file_records[r].get("done"):
                self.table.removeRow(r)
                self.file_records.pop(r)
        self.summary_lbl.setText(f"已清理完成项 · 剩余 {len(self.file_records)} 个任务")

    def _clear_all_rows(self) -> None:
        if self.worker and self.worker.isRunning():
            return
        self.table.setRowCount(0)
        self.file_records.clear()
        self.batch_bar.setValue(0)
        self.summary_lbl.setText("列表已清空")

    # -----------------------------------------------------------------------
    # 输出目录设置
    # -----------------------------------------------------------------------
    def _on_out_mode_changed(self, idx: int) -> None:
        custom = idx == 1
        self.out_dir_edit.setEnabled(custom)
        self.btn_pick_dir.setEnabled(custom)
        self._save_settings()

    def _pick_output_dir(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "选择输出文件夹", self.out_dir_edit.text().strip())
        if d:
            self.out_dir_edit.setText(d)
            self._save_settings()

    def _open_current_output_dir(self) -> None:
        if self.out_mode_combo.currentIndex() == 1 and self.out_dir_edit.text().strip():
            p = Path(self.out_dir_edit.text().strip())
            p.mkdir(parents=True, exist_ok=True)
            reveal_in_file_manager(p)
            return
        if self.file_records:
            last = self.file_records[-1]
            out_s = str(last.get("out_path", ""))
            reveal_in_file_manager(Path(out_s) if out_s else Path(str(last["in_path"])).parent)
        else:
            reveal_in_file_manager(get_app_dir())

    # -----------------------------------------------------------------------
    # 启动与中止批量转换
    # -----------------------------------------------------------------------
    def _on_start_or_stop_clicked(self) -> None:
        if self.worker and self.worker.isRunning():
            self.worker.request_cancel()
            self.start_btn.setEnabled(False)
            self.start_btn.setText("正在中止...")
            return

        if not self.file_records:
            QMessageBox.information(self, "列表为空", "请先添加或拖入需要转换压缩的文件。")
            return

        out_dir = ""
        if self.out_mode_combo.currentIndex() == 1:
            out_dir = self.out_dir_edit.text().strip()
            if not out_dir:
                QMessageBox.warning(self, "未指定输出目录", "请选择统一保存的目标文件夹，或切换回保存在原文件目录。")
                return

        tasks: List[TaskItem] = []
        for row_idx, rec in enumerate(self.file_records):
            rec["done"] = False
            rec["out_path"] = ""
            bar = self.table.cellWidget(row_idx, 3)
            if isinstance(bar, QProgressBar):
                bar.setValue(0)
            self.table.item(row_idx, 2).setText("—")
            self.table.item(row_idx, 4).setText("⏳ 排队中")
            tasks.append(TaskItem(row_id=row_idx, in_path=Path(str(rec["in_path"])), mode=self.current_mode))

        options: Dict[str, object] = {
            "out_dir": out_dir,
            "target_fmt": self.target_fmt_combo.currentText().strip(),
            "comp_level": self.comp_combo.currentData(),
            "img_max_dim": self.img_dim_combo.currentData(),
            "img_strip_exif": self.img_strip_exif_chk.isChecked(),
            "aud_sample_rate": self.aud_sr_combo.currentData(),
            "aud_loudnorm": self.aud_loudnorm_chk.isChecked(),
            "vid_max_width": self.vid_res_combo.currentData(),
            "vid_codec": self.vid_codec_combo.currentData(),
            "vid_mute": self.vid_mute_chk.isChecked(),
            "nov_indent": self.nov_indent_chk.isChecked(),
            "nov_pdf_preset": self.nov_pdf_combo.currentData(),
            "nov_txt_enc": self.nov_enc_combo.currentData(),
            "nov_author": self.nov_author_edit.text().strip(),
        }

        self.batch_bar.setRange(0, len(tasks) * 100)
        self.batch_bar.setValue(0)
        self._completed_count = 0
        self._total_tasks = len(tasks)

        self.start_btn.setObjectName("StopBtn")
        self.start_btn.setText("⏹ 停止转换")
        self._apply_theme()

        self.worker = BatchConversionWorker(tasks=tasks, options=options, parent=self)
        self.worker.task_progress.connect(self._on_task_progress)
        self.worker.task_finished.connect(self._on_task_finished)
        self.worker.batch_finished.connect(self._on_batch_finished)
        self.worker.start()

    def _on_task_progress(self, row_id: int, pct: int, status_text: str) -> None:
        if 0 <= row_id < self.table.rowCount():
            bar = self.table.cellWidget(row_id, 3)
            if isinstance(bar, QProgressBar):
                bar.setValue(pct)
            self.table.item(row_id, 4).setText(status_text)
            self.batch_bar.setValue(self._completed_count * 100 + pct)
            self.summary_lbl.setText(
                f"正在处理 ({self._completed_count + 1}/{self._total_tasks})：{self.table.item(row_id, 0).text()}"
            )

    def _on_task_finished(self, row_id: int, ok: bool, out_path_str: str, msg: str, out_bytes: int) -> None:
        self._completed_count += 1
        self.batch_bar.setValue(self._completed_count * 100)

        if 0 <= row_id < len(self.file_records):
            rec = self.file_records[row_id]
            in_size = int(rec["in_size"])  # type: ignore[arg-type]
            bar = self.table.cellWidget(row_id, 3)
            if isinstance(bar, QProgressBar):
                bar.setValue(100 if ok else 0)

            st_item = self.table.item(row_id, 4)
            st_item.setText(msg if len(msg) <= 28 else msg[:26] + "...")
            st_item.setToolTip(msg)

            if ok and out_bytes > 0:
                rec["done"] = True
                rec["out_path"] = out_path_str
                after_item = self.table.item(row_id, 2)
                if in_size > 0:
                    ratio = (out_bytes - in_size) / in_size * 100.0
                    sign_str = f"{ratio:+.1f}%"
                    after_item.setText(f"{format_size(out_bytes)} ({sign_str})")
                    if ratio < 0:
                        after_item.setForeground(QColor("#10b981"))
                else:
                    after_item.setText(format_size(out_bytes))

    def _on_batch_finished(self, success_cnt: int, fail_cnt: int, saved_bytes: int) -> None:
        self.start_btn.setEnabled(True)
        self.start_btn.setObjectName("PrimaryBtn")
        self.start_btn.setText("🚀 开始批量转换")
        self._apply_theme()

        saved_tip = f" · 累计节省空间 {format_size(saved_bytes)}" if saved_bytes > 0 else ""
        self.summary_lbl.setText(
            f"🎉 批量任务结束：成功 {success_cnt} 个，失败/中止 {fail_cnt} 个{saved_tip}（双击已完成行可定位文件）"
        )


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    app.setApplicationName("OmniConvert")
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()