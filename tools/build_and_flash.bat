@echo off
rem ============================================================
rem  build_and_flash.bat -- one click:
rem    1) build F:\DongleRE\firmware
rem    2) check the app image really contains the new version string
rem    3) re-generate the WEB FLASH PACKAGE (webgui\firmware\manifest.json)
rem    4) validate the package with tools\test_fw_pack.py
rem  Then YOU flash from the web page (or rerun with -SerialFlash).
rem
rem  Usage:
rem    double-click this file                (build + pack + validate)
rem    build_and_flash.bat -SerialFlash      (also flash over UART; dongle in BOOT)
rem    build_and_flash.bat -SkipBuild        (only re-pack)
rem    build_and_flash.bat -OnlyFlash        (only flash what is already built)
rem ============================================================
setlocal
cd /d "%~dp0.."

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_and_flash.ps1" %*
set RC=%ERRORLEVEL%

echo.
if not "%RC%"=="0" (
    echo [FAILED] exit=%RC%  -- read the messages above.
) else (
    echo [DONE] read the NEXT block above.
)
echo.
pause
exit /b %RC%
