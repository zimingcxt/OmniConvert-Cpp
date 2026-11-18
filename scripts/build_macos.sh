#!/usr/bin/env bash
set -euo pipefail

# OmniConvert C++/Qt6 — macOS 离线便携包一键构建脚本
# 用法:
#   ./scripts/build_macos.sh [QT6_PREFIX_PATH]
# 示例:
#   ./scripts/build_macos.sh /opt/homebrew/opt/qt
#   ./scripts/build_macos.sh ~/Qt/6.7.3/macos

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/macos-release"
DIST_APP="${PROJECT_ROOT}/OmniConvert.app"

QT_PREFIX="${1:-${CMAKE_PREFIX_PATH:-}}"
if [[ -z "${QT_PREFIX}" ]]; then
    for cand in /opt/homebrew/opt/qt /usr/local/opt/qt "$HOME"/Qt/6.*/macos; do
        if [[ -d "${cand}" ]]; then
            QT_PREFIX="${cand}"
            break
        fi
    done
fi

CMAKE_ARGS=(-S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ -n "${QT_PREFIX}" ]]; then
    echo "==> Using Qt6 SDK at: ${QT_PREFIX}"
    CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX}")
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --config Release -j"$(sysctl -n hw.ncpu)"

echo "==> Running automated test suite..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

rm -rf "${DIST_APP}"
cp -R "${BUILD_DIR}/OmniConvert.app" "${DIST_APP}"

# Bundle Qt6 frameworks & imageformat plugins for standalone offline execution
MACDEPLOYQT=""
if [[ -n "${QT_PREFIX}" && -x "${QT_PREFIX}/bin/macdeployqt" ]]; then
    MACDEPLOYQT="${QT_PREFIX}/bin/macdeployqt"
elif command -v macdeployqt >/dev/null 2>&1; then
    MACDEPLOYQT="$(command -v macdeployqt)"
fi

if [[ -n "${MACDEPLOYQT}" ]]; then
    echo "==> Embedding Qt6 frameworks & plugins via macdeployqt..."
    "${MACDEPLOYQT}" "${DIST_APP}" -always-overwrite
fi

# Bundle portable ffmpeg binary if present in ./bin/ffmpeg
if [[ -f "${PROJECT_ROOT}/bin/ffmpeg" ]]; then
    mkdir -p "${DIST_APP}/Contents/MacOS/bin"
    cp -f "${PROJECT_ROOT}/bin/ffmpeg" "${DIST_APP}/Contents/MacOS/bin/ffmpeg"
    chmod +x "${DIST_APP}/Contents/MacOS/bin/ffmpeg"
fi

echo "==> Ad-hoc code signing OmniConvert.app for Apple Silicon / Gatekeeper compatibility..."
codesign --force --deep --sign - "${DIST_APP}"

echo "✅ Build complete: ${DIST_APP}"
