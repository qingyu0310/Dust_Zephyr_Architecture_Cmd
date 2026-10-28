@echo off
setlocal
rem dust CLI - subcommand dispatch
rem   dust build <board> [extra west build args]
rem   dust flash [extra west flash args]
rem   dust create template
rem
rem dispatch via goto, not if(...) block: exit /b inside a block loses errorlevel

if /i "%~1"=="build"  goto :do_build
if /i "%~1"=="flash"  goto :do_flash
if /i "%~1"=="create" goto :do_create
goto :usage

:do_build
call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%

:do_flash
call "%~dp0flash.bat" %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%

:do_create
call "%~dp0..\create\create.bat" %2
exit /b %errorlevel%

:usage
echo Usage: dust build ^<board^> [extra west build args]
echo        dust flash [extra west flash args]
echo        dust create template
exit /b 1
