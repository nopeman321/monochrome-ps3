@echo off
title Monochrome - Lossless Music Player (PC Native)
cd /d "%~dp0"

if not exist "bin\monochrome_pc.exe" (
    echo [INFO] Compiling native PC client...
    call build_pc.bat
)

if exist "bin\monochrome_pc.exe" (
    start "" "bin\monochrome_pc.exe"
    exit /b 0
)

echo [ERROR] Failed to find or compile bin\monochrome_pc.exe
pause
exit /b 1
