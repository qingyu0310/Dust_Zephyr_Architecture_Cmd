@echo off
setlocal enabledelayedexpansion

set NAME=%1
if "%NAME%"=="" set NAME=hpm6e00evk

set BOARD=
for /d %%d in (projects\boards\*) do (
    if exist "%%d\%NAME%\*.overlay" (
        for %%f in ("%%d\%NAME%\*.overlay") do set BOARD=%%~nf
    )
)

if not "%BOARD%"=="" (
    west build -b !BOARD! %2 %3 %4 %5 %6 %7 %8 %9 -- -DBOARD_CFG=%NAME%
) else (
    west build -b %NAME% %2 %3 %4 %5 %6 %7 %8 %9
)


