@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   MONOCHROME - Launching Native Desktop Music Player
echo ========================================================

if not exist "bin\monochrome_pc.exe" (
    call build_pc.bat
)

if exist "bin\monochrome_pc.exe" (
    start "" "bin\monochrome_pc.exe"
    exit /b 0
)

echo [ERROR] Failed to start native player!
pause
exit /b 1
