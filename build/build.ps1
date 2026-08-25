param(
    [string]$Name = "hpm6e00evk",
    [string]$Opts = ""
)

# 固定 SDK_GLUE_DIR（覆盖系统可能存在的脏环境变量）
$env:SDK_GLUE_DIR = "E:\Zephyr_HPMicro\sdk_glue"

# 定位目标项目：在 zephyr_user 下→维护者 project；当前目录已是项目根→当前目录；否则默认维护者
# build 产物放定位到的项目目录 build\ 下，git 上传时忽略
$__cwd = (Get-Location).Path
$targetDir = $null
if (Test-Path (Join-Path $__cwd "CMakeLists.txt")) {
    $targetDir = $__cwd
} elseif (Test-Path (Join-Path $__cwd "project\CMakeLists.txt")) {
    $targetDir = Join-Path $__cwd "project"
}

if ($null -eq $targetDir) {
    Write-Error @"
dust build could not locate a project root from:
  current dir: $__cwd
  expected one of:
  - $__cwd\CMakeLists.txt
  - $__cwd\project\CMakeLists.txt
"@
    exit 1
}

Set-Location $targetDir

switch ($Name) {
    "board_rm_c" {
        west build -b stm32f4_disco $Opts -- -DBOARD_CFG=$Name
    }
    default {
        west build -b $Name $Opts
    }
}
