@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   MONOCHROME - Launching Desktop Music Client
echo ========================================================

if exist "bin\monochrome_pc.exe" (
    start "" "bin\monochrome_pc.exe"
    exit /b 0
)

where pythonw >nul 2>&1
if %ERRORLEVEL% equ 0 (
    start "" pythonw tools\desktop_app.py
    exit /b 0
)

where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
    start "" python tools\desktop_app.py
    exit /b 0
)

call build_pc.bat
start "" "bin\monochrome_pc.exe"
exit /b 0
