@echo off
setlocal
rem dust CLI — 子命令分发
rem   dust build <board> [extra west build args]
rem   dust flash [extra west flash args]
if /i "%~1"=="build" (
    call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
if /i "%~1"=="flash" (
    call "%~dp0flash.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
echo Usage: dust build ^<board^> [extra west build args]
echo        dust flash [extra west flash args]
exit /b 1
