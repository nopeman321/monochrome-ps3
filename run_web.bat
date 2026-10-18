@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   MONOCHROME - Launching Desktop Web Client
echo ========================================================

python tools\desktop_app.py
