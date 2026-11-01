<#
.SYNOPSIS
    扫描打包目录里可执行文件的 DLL 依赖，找出既不在包内、也不在系统目录的缺失项。

.DESCRIPTION
    目标机没装 Qt 时，最常见的翻车原因是某个 DLL 或 Qt 插件没被 windeployqt 收进来。
    开发机上跑得好，只因为 Qt 恰好在 PATH 上——这证明不了产物可用。
    这里用静态导入表扫描代替"再找一台干净电脑"：从 exe 出发逐层解析 /dependents，
    依赖要么在包目录里，要么是 Windows 系统自带，否则就是缺。
    系统 DLL 只判定不再向下递归，免得把 Windows 自己的依赖链也刷屏。

.EXAMPLE
    powershell -File scripts/check-deps.ps1 -Stage dist/SynthForgeStudio
#>
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$Exe = "synthforge_app.exe",
    # 不传就按产物自己判：GUI 必须带 WebEngine 那几件，无头 CLI 不需要
    [string[]]$Required = $null
)

$ErrorActionPreference = 'Stop'
$stageRoot = (Resolve-Path -LiteralPath $Stage).Path
$systemDir = Join-Path $env:WINDIR "System32"

# 光有 DLL 名字齐还不够：Qt 插件与资源要按相对路径查找，缺了就白屏/黑屏。
# 写成局部变量而不当默认参数：`-File` 传空数组会被当成“缺参数”而报错。
$guiRequired = @(
    "platforms\qwindows.dll",
    "Qt6WebEngineCore.dll",
    "QtWebEngineProcess.exe",
    "resources",
    "translations\qtwebengine_locales"
)
if ($null -eq $Required) {
    $Required = if ($Exe -match 'synthforge_app') { $guiRequired } else { @() }
}

function Find-DumpBin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($root in @("C:\Program Files\Microsoft Visual Studio",
                        "C:\Program Files (x86)\Microsoft Visual Studio")) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\Hostx64\\x64\\dumpbin\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw "找不到 dumpbin.exe：请在 Developer PowerShell 里运行，或确认 VS 的 C++ 工具链已安装。"
}

# dumpbin 输出里只有依赖行本身是 "xxx.dll" 独占一行，其余行不会误匹配
function Get-DirectDependencies([string]$path, [string]$dumpBin) {
    & $dumpBin /nologo /dependents $path 2>$null |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '^[A-Za-z0-9_.\-]+\.dll$' }
}

$dumpBin = Find-DumpBin
Write-Host "dumpbin: $dumpBin"
Write-Host "扫描: $stageRoot\$Exe"

$queue = New-Object System.Collections.Queue
$queue.Enqueue((Join-Path $stageRoot $Exe))
$visited = @{}
$missing = @()
$checked = 0

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    if (-not (Test-Path -LiteralPath $current)) { $missing += "文件不存在：$current"; continue }
    $checked++
    foreach ($dep in (Get-DirectDependencies $current $dumpBin)) {
        $key = $dep.ToLower()
        if ($visited.ContainsKey($key)) { continue }
        $visited[$key] = $true
        # api-ms-win-* / ext-ms-* 是 Windows 的 API Set：磁盘上没有对应文件，由加载器内联解析，
        # 它们既不在包里也不在 System32，但并不代表缺依赖
        if ($dep -match '^(api-ms-win|ext-ms)-') { continue }
        $inStage = Get-ChildItem -Path $stageRoot -Filter $dep -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($inStage) { $queue.Enqueue($inStage.FullName); continue }
        if (Test-Path -LiteralPath (Join-Path $systemDir $dep)) { continue }  # Windows 自带
        $missing += $dep
    }
}

# 双击不得弹黑框：GUI 产物必须是 Windows GUI 子系统。
# 这里不是可选项：忘了开 WIN32_EXECUTABLE 就会多弹一个黑底控制台窗口，不双击看根本发现不了。
if ($Exe -match 'synthforge_app') {
    $headers = & $dumpBin /nologo /headers (Join-Path $stageRoot $Exe) 2>$null |
        Select-String -Pattern "subsystem \("
    $shown = ($headers | ForEach-Object { $_.ToString().Trim() }) -join " "
    if ($shown -notmatch "Windows GUI") {
        $missing += "产物是控制台子系统（双击会弹黑框），需要 set_target_properties(... WIN32_EXECUTABLE TRUE)；实际：$shown"
    }
}

foreach ($rel in $Required) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $rel))) {
        $missing += "缺少打包成员：$rel"
    }
}

Write-Host "解析了 $checked 个模块，共 $($visited.Count) 个依赖名。"
if ($missing.Count -gt 0) {
    Write-Host "`n缺失项（目标机上会起不来或白屏）：" -ForegroundColor Red
    $missing | Sort-Object -Unique | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host "`n若缺的是 vcruntime140.dll / msvcp140.dll：目标机需要装 VC++ 2015-2022 x64 运行库。"
    exit 1
}

Write-Host "依赖齐全。" -ForegroundColor Green
