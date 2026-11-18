<#
.SYNOPSIS
    OmniConvert C++/Qt6 — Windows (x64) 离线绿色便携版一键构建脚本
.EXAMPLE
    .\scripts\build_windows.ps1 -QtPrefix "C:\Qt\6.7.3\msvc2022_64" -FfmpegExe "C:\ffmpeg\bin\ffmpeg.exe"
#>
param(
    [string]$QtPrefix = $env:CMAKE_PREFIX_PATH,
    [string]$FfmpegExe = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build\win-release"
$DistDir = Join-Path $ProjectRoot "dist\OmniConvert-Win64-Portable"

if (-not $QtPrefix) {
    $Candidates = Get-ChildItem -Path "C:\Qt\6*\msvc*_64" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    if ($Candidates) {
        $QtPrefix = $Candidates[0].FullName
    }
}

Write-Host "==> Configuring OmniConvert (Windows x64 Release)..." -ForegroundColor Cyan
$CmakeArgs = @("-S", $ProjectRoot, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
if ($QtPrefix) {
    Write-Host "==> Using Qt6 SDK: $QtPrefix" -ForegroundColor Green
    $CmakeArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix"
}

cmake @CmakeArgs
cmake --build $BuildDir --config Release --parallel

Write-Host "==> Assembling standalone Windows portable folder..." -ForegroundColor Cyan
if (Test-Path $DistDir) { Remove-Item -Recurse -Force $DistDir }
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $DistDir "bin") | Out-Null

$ExeCandidate = Join-Path $BuildDir "Release\OmniConvert.exe"
if (-not (Test-Path $ExeCandidate)) {
    $ExeCandidate = Join-Path $BuildDir "OmniConvert.exe"
}
Copy-Item -Force $ExeCandidate (Join-Path $DistDir "OmniConvert.exe")

# Run windeployqt to bundle all Qt6 DLLs and imageformats plugins (qjpeg, qwebp, qtiff, qico, qgif)
$WinDeployQt = if ($QtPrefix) { Join-Path $QtPrefix "bin\windeployqt.exe" } else { "windeployqt.exe" }
& $WinDeployQt --release --no-translations --compiler-runtime (Join-Path $DistDir "OmniConvert.exe")

# Bundle offline ffmpeg.exe if provided or present in .\bin\ffmpeg.exe
if (-not $FfmpegExe -and (Test-Path (Join-Path $ProjectRoot "bin\ffmpeg.exe"))) {
    $FfmpegExe = Join-Path $ProjectRoot "bin\ffmpeg.exe"
}
if ($FfmpegExe -and (Test-Path $FfmpegExe)) {
    Copy-Item -Force $FfmpegExe (Join-Path $DistDir "bin\ffmpeg.exe")
    Write-Host "==> Bundled offline ffmpeg.exe into dist\OmniConvert-Win64-Portable\bin\ffmpeg.exe" -ForegroundColor Green
}

$ZipPath = Join-Path $ProjectRoot "dist\OmniConvert-Win64-Portable.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path "$DistDir\*" -DestinationPath $ZipPath

Write-Host "✅ Windows Offline Portable Build Ready: $DistDir" -ForegroundColor Green
Write-Host "✅ Portable Archive: $ZipPath" -ForegroundColor Green
