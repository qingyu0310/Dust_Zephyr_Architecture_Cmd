@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem 固定 ZEPHYR_BASE 与 SDK_GLUE_DIR（覆盖环境变量缺失/脏值）
set "ZEPHYR_BASE=E:\Zephyr\zephyr"
set "SDK_GLUE_DIR=E:\Zephyr_HPMicro\sdk_glue"

set NAME=%1
if "%NAME%"=="" set NAME=hpm6e00evk

set "TARGET_DIR="
if exist "%CD%\CMakeLists.txt" (
    set "TARGET_DIR=%CD%"
) else if exist "%CD%\project\CMakeLists.txt" (
    set "TARGET_DIR=%CD%\project"
)

if "%TARGET_DIR%"=="" (
    echo [ERROR] dust build could not locate a project root from:
    echo         current dir: %CD%
    echo         expected one of:
    echo         - %CD%\CMakeLists.txt
    echo         - %CD%\project\CMakeLists.txt
    exit /b 1
)

pushd "%TARGET_DIR%"

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

set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
