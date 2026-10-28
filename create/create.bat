@echo off
chcp 65001 >nul
setlocal

rem dust create - scaffold the current user area (six modules + project wiring)
rem   dust create template
rem   template name is fixed; target = current user area root, no new dir

if /i not "%~1"=="template" (
    echo Usage: dust create template
    exit /b 1
)

set "SKELETON=%~dp0skeleton"
if not exist "%SKELETON%\project\CMakeLists.txt" (
    echo [ERROR] skeleton not found: %SKELETON%
    exit /b 1
)

rem locate user area root:
rem   - cwd is named "project"  -> parent is the root (running inside project\)
rem   - otherwise               -> cwd is the root (empty dir is fine)
set "USER_ROOT=%CD%"
for %%d in ("%CD%") do if /i "%%~nxd"=="project" (
    for %%p in ("%CD%\..") do set "USER_ROOT=%%~fp"
)

rem copy whole tree: skeleton files overwrite same-name files (wiring),
rem business files (thread\*\*.cpp, boards\*) are not in skeleton and stay untouched
xcopy /E /I /Y /H "%SKELETON%\*" "%USER_ROOT%" >nul
if errorlevel 1 (
    echo [ERROR] copy failed: %SKELETON% -^> %USER_ROOT%
    exit /b 1
)

echo [dust] skeleton created in %USER_ROOT%
exit /b 0
