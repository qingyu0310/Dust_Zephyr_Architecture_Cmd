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

# 打印生效配置（只读，不参与烧录）
$runnersYaml = "build\zephyr\runners.yaml"
if (Test-Path $runnersYaml) {
    Write-Host "[dust] project root : $targetDir"
    $board = (Select-String -Path "build\CMakeCache.txt" -Pattern '^BOARD:STRING=(.*)$').Matches.Groups[1].Value
    $runner = (Select-String -Path $runnersYaml -Pattern '^flash-runner:\s*(.*)$').Matches.Groups[1].Value
    $openocd = (Select-String -Path $runnersYaml -Pattern '^  openocd:\s*(.*)$').Matches.Groups[1].Value
    Write-Host "[dust] board        : $board"
    Write-Host "[dust] flash runner : $runner"
    Write-Host "[dust] openocd      : $openocd"
} else {
    Write-Host '[dust] no build\zephyr\runners.yaml -- run "dust build" first'
}

west flash $Opts
