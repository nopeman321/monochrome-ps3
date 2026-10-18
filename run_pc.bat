@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   MONOCHROME - Launching Desktop Music Client
echo ========================================================

where pythonw >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [INFO] Starting Monochrome Desktop App (Full Hi-Res Web UI)...
    start "" pythonw tools\desktop_app.py
    exit /b 0
)

where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [INFO] Starting Monochrome Desktop App (Full Hi-Res Web UI)...
    start "" python tools\desktop_app.py
    exit /b 0
)

if exist "bin\monochrome_pc.exe" (
    echo [INFO] Starting Native Win32 Client...
    start "" "bin\monochrome_pc.exe"
    exit /b 0
)

echo [INFO] Compiling native PC client...
call build_pc.bat
start "" "bin\monochrome_pc.exe"
