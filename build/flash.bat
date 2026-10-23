@echo off
chcp 65001 >nul

rem 固定 ZEPHYR_BASE 与 SDK_GLUE_DIR（覆盖环境变量缺失/脏值）
set "ZEPHYR_BASE=E:\Zephyr\zephyr"
set "SDK_GLUE_DIR=E:\Zephyr_HPMicro\sdk_glue"

set "TARGET_DIR="
if exist "%CD%\CMakeLists.txt" (
    set "TARGET_DIR=%CD%"
) else if exist "%CD%\project\CMakeLists.txt" (
    set "TARGET_DIR=%CD%\project"
)

if "%TARGET_DIR%"=="" (
    echo [ERROR] dust flash could not locate a project root from:
    echo         current dir: %CD%
    echo         expected one of:
    echo         - %CD%\CMakeLists.txt
    echo         - %CD%\project\CMakeLists.txt
    exit /b 1
)

pushd "%TARGET_DIR%"

rem 打印生效配置（只读，不参与烧录）
if exist "build\zephyr\runners.yaml" (
    echo [dust] project root : %CD%
    for /f "tokens=1,* delims==" %%a in ('findstr /b /c:"BOARD:STRING=" build\CMakeCache.txt') do echo [dust] board        : %%b
    for /f "tokens=1,* delims=: " %%a in ('findstr /b /c:"flash-runner:" build\zephyr\runners.yaml') do echo [dust] flash runner : %%b
    for /f "tokens=1,* delims=: " %%a in ('findstr /b /c:"  openocd: " build\zephyr\runners.yaml') do echo [dust] openocd      : %%b
) else (
    echo [dust] no build\zephyr\runners.yaml -- run "dust build" first
)

west flash %*

set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
