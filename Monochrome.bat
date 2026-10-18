@echo off
title Monochrome - Lossless Music Player
cd /d "%~dp0"

where pythonw >nul 2>&1
if %ERRORLEVEL% equ 0 (
    start "" pythonw tools\desktop_app.py
    exit
)

where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
    start "" python tools\desktop_app.py
    exit
)

if exist "bin\monochrome_pc.exe" (
    start "" "bin\monochrome_pc.exe"
    exit
)

echo [INFO] Compiling launcher...
call build_pc.bat
start "" "bin\monochrome_pc.exe"
exit
