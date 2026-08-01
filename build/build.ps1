param(
    [string]$Name = "hpm6e00evk",
    [string]$Opts = ""
)

# 固定 SDK_GLUE_DIR（覆盖系统可能存在的脏环境变量）
$env:SDK_GLUE_DIR = "E:\Zephyr_HPMicro\sdk_glue"

# 定位目标项目：在 zephyr_user 下→维护者 project；当前目录已是项目根→当前目录；否则默认维护者
# build 产物放定位到的项目目录 build\ 下，git 上传时忽略
$__cwd = (Get-Location).Path
if ($__cwd -eq "E:\Zephyr\zephyr_user") {
    Set-Location "E:\Zephyr\zephyr_user\project"
} elseif (Test-Path "$__cwd\CMakeLists.txt") {
    Set-Location $__cwd
} else {
    Set-Location "E:\Zephyr\zephyr_user\project"
}

switch ($Name) {
    "board_rm_c" {
        west build -b stm32f4_disco $Opts -- -DBOARD_CFG=$Name
    }
    default {
        west build -b $Name $Opts
    }
}
