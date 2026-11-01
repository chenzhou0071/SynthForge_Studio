<#
.SYNOPSIS
    产出可分发的 SynthForge Studio 目录：前端构建 → CMake Release → windeployqt → 依赖自检。

.DESCRIPTION
    产物只依赖目标机的 Windows 系统组件与 VC++ 运行库；web/dist 已编进 qrc，无需外置静态文件。
    最后一步强制跑 check-deps：依赖不齐就当场失败，避免交出一个"在我电脑上能跑"的包。

.PARAMETER QtKit
    Qt 的 msvc 目录，例如 C:\Qt\6.10.3\msvc2022_64。

.EXAMPLE
    powershell -File scripts/package.ps1 -QtKit C:\Qt\6.10.3\msvc2022_64
#>
param(
    [Parameter(Mandatory = $true)][string]$QtKit,
    [string]$Config = "Release",
    [string]$Stage = "dist\SynthForgeStudio"
)

$ErrorActionPreference = 'Stop'
Set-Location (git rev-parse --show-toplevel)

if (-not (Test-Path (Join-Path $QtKit "bin\windeployqt.exe"))) {
    throw "QtKit 下没有 bin\windeployqt.exe：$QtKit"
}

Write-Host "`n[1/4] 前端构建" -ForegroundColor Cyan
Push-Location web
# CI 上是干净环境，必须 npm ci 拉完整依赖；本地已经装过就直接建，不白耗几分钟
if (-not (Test-Path "node_modules")) { npm ci }
npm run build
Pop-Location

Write-Host "`n[2/4] CMake $Config 构建" -ForegroundColor Cyan
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$QtKit"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败。" }
cmake --build build --config $Config --target synthforge_app
if ($LASTEXITCODE -ne 0) { throw "编译失败。" }

Write-Host "`n[3/4] 部署到 $Stage" -ForegroundColor Cyan
# 只清本脚本自己的产物目录：路径不在 dist\ 下就直接拒绝，防止误删
$stageFull = [System.IO.Path]::GetFullPath($Stage)
if ($stageFull -notmatch '\\dist\\') { throw "拒绝清理非 dist 目录：$stageFull" }
if (Test-Path $stageFull) { Remove-Item -Recurse -Force $stageFull }
New-Item -ItemType Directory -Path $stageFull | Out-Null
Copy-Item "build\$Config\synthforge_app.exe" $stageFull

& (Join-Path $QtKit "bin\windeployqt.exe") --$($Config.ToLower()) --no-translations (Join-Path $stageFull "synthforge_app.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败，退出码 $LASTEXITCODE。" }

Write-Host "`n[4/4] 依赖自检" -ForegroundColor Cyan
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-deps.ps1 -Stage $stageFull
if ($LASTEXITCODE -ne 0) { throw "依赖自检未通过，产物不可分发。" }

Write-Host "`n打包完成：$stageFull" -ForegroundColor Green
Write-Host "目标机若缺 vcruntime140.dll，装 VC++ 2015-2022 x64 可再发行组件即可。"
