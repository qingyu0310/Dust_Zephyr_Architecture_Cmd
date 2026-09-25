param(
    [string]$Opts = ""
)

# 固定 SDK_GLUE_DIR（覆盖系统可能存在的脏环境变量）
$env:SDK_GLUE_DIR = "E:\Zephyr_HPMicro\sdk_glue"

# 定位目标项目：当前目录已是项目根→当前目录；工作区根→其下 project；否则报错
$__cwd = (Get-Location).Path
$targetDir = $null
if (Test-Path (Join-Path $__cwd "CMakeLists.txt")) {
    $targetDir = $__cwd
} elseif (Test-Path (Join-Path $__cwd "project\CMakeLists.txt")) {
    $targetDir = Join-Path $__cwd "project"
}

if ($null -eq $targetDir) {
    Write-Error @"
dust flash could not locate a project root from:
  current dir: $__cwd
  expected one of:
  - $__cwd\CMakeLists.txt
  - $__cwd\project\CMakeLists.txt
"@
    exit 1
}

Set-Location $targetDir

west flash $Opts
