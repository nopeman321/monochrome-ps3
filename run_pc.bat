@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   MONOCHROME - Launching Native PC Client (Windows)
echo ========================================================

if not exist "bin\monochrome_pc.exe" (
    echo [INFO] Compiling native PC client first...
    call build_pc.bat
)

if not exist "bin\monochrome_pc.exe" (
    echo [ERROR] Could not build bin\monochrome_pc.exe!
    pause
    exit /b 1
)

start "" "bin\monochrome_pc.exe"
echo [SUCCESS] Monochrome PC Client launched!
