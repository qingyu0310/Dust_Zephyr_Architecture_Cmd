@echo off
setlocal
rem dust CLI — 子命令分发：dust build <board> [extra west build args]
if /i "%~1"=="build" (
    call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
echo Usage: dust build ^<board^> [extra west build args]
exit /b 1
