@echo off
title Monochrome PS3 - RPCS3 Emulator Launcher
echo ========================================================
echo   Iniciando Monochrome Music en RPCS3 (Cell B.E.)...
echo ========================================================

set SCRIPT_DIR=%~dp0
set EBOOT=%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001\USRDIR\EBOOT.BIN

if not exist "%EBOOT%" (
    echo [INFO] Desplegando EBOOT.BIN a RPCS3...
    call "%SCRIPT_DIR%build_homebrew.bat"
)

cd /d "%SCRIPT_DIR%emulator\rpcs3"
if exist "%EBOOT%" (
    start "" rpcs3.exe "%EBOOT%"
) else (
    start "" rpcs3.exe
)
exit
