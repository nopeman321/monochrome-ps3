@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   MONOCHROME - Host PC Native Build System (Windows)
echo ========================================================

set SCRIPT_DIR=%~dp0
set ZIG=%SCRIPT_DIR%tools\zig\zig.exe

if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"

echo [1/1] Compiling native Windows Desktop Client (monochrome_pc.exe)...
"%ZIG%" cc -O2 -I"%SCRIPT_DIR%include" "%SCRIPT_DIR%src\main.c" "%SCRIPT_DIR%src\ui.c" "%SCRIPT_DIR%src\audio.c" "%SCRIPT_DIR%src\network.c" "%SCRIPT_DIR%src\cover.c" "%SCRIPT_DIR%src\cJSON.c" "%SCRIPT_DIR%src\flac_decoder.c" "%SCRIPT_DIR%src\platform_pc.c" -lwinhttp -lwinmm -lgdi32 -luser32 "-Wl,--subsystem,console" -o "%SCRIPT_DIR%bin\monochrome_pc.exe"
if errorlevel 1 (
    echo [ERROR] Desktop compilation failed!
    exit /b 1
)

echo ========================================================
echo   [SUCCESS] Executable ready for Host PC (Pure Native):
echo   %SCRIPT_DIR%bin\monochrome_pc.exe
echo   %SCRIPT_DIR%Monochrome.bat
echo ========================================================
