@echo off
setlocal enabledelayedexpansion

rem 固定 ZEPHYR_BASE 与 SDK_GLUE_DIR（覆盖环境变量缺失/脏值）
set "ZEPHYR_BASE=E:\Zephyr\zephyr"
set "SDK_GLUE_DIR=E:\Zephyr_HPMicro\sdk_glue"

set NAME=%1
if "%NAME%"=="" set NAME=hpm6e00evk

rem 直接检测：当前目录必须是项目根（含 CMakeLists.txt），build 产物放当前目录 build\
if not exist "%CD%\CMakeLists.txt" (
    echo [ERROR] 当前目录不是项目根（缺少 CMakeLists.txt）
    echo         请先 cd 到项目根（zephyr_user\project 或 projects\name）再运行本脚本
    exit /b 1
)

set BOARD=
for /d %%d in (boards\*) do (
    if exist "%%d\%NAME%\*.overlay" (
        for %%f in ("%%d\%NAME%\*.overlay") do set BOARD=%%~nf
    )
)

if not "%BOARD%"=="" (
    west build -b !BOARD! %2 %3 %4 %5 %6 %7 %8 %9 -- -DBOARD_CFG=%NAME%
) else (
    west build -b %NAME% %2 %3 %4 %5 %6 %7 %8 %9
)


