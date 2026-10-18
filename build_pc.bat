@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   MONOCHROME - Host PC Native Build System (Windows)
echo ========================================================

set SCRIPT_DIR=%~dp0
set ZIG=%SCRIPT_DIR%tools\zig\zig.exe

if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"

echo [1/1] Compiling native Windows client with Zig C99...
"%ZIG%" cc -O2 -I"%SCRIPT_DIR%include" "%SCRIPT_DIR%src\main.c" "%SCRIPT_DIR%src\ui.c" "%SCRIPT_DIR%src\audio.c" "%SCRIPT_DIR%src\network.c" "%SCRIPT_DIR%src\cJSON.c" "%SCRIPT_DIR%src\flac_decoder.c" "%SCRIPT_DIR%src\platform_pc.c" -lwinhttp -lwinmm -lgdi32 -luser32 -lws2_32 -o "%SCRIPT_DIR%bin\monochrome_pc.exe"

if errorlevel 1 (
    echo [ERROR] Native PC compilation failed!
    exit /b 1
)

echo ========================================================
echo   [SUCCESS] Executable ready for Host PC:
echo   %SCRIPT_DIR%bin\monochrome_pc.exe
echo ========================================================
