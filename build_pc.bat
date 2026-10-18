@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   MONOCHROME - Host PC Native Build System (Windows)
echo ========================================================

set SCRIPT_DIR=%~dp0
set ZIG=%SCRIPT_DIR%tools\zig\zig.exe

if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"

echo [1/2] Compiling native Windows Desktop Client (monochrome_pc.exe)...
"%ZIG%" cc -O2 "%SCRIPT_DIR%tools\launcher.c" -luser32 -lshell32 "-Wl,--subsystem,windows" -o "%SCRIPT_DIR%bin\monochrome_pc.exe"
if errorlevel 1 (
    echo [ERROR] Desktop launcher compilation failed!
    exit /b 1
)

echo [2/2] Compiling low-level C99 embedded core (monochrome_c99_core.exe)...
"%ZIG%" cc -O2 -I"%SCRIPT_DIR%include" "%SCRIPT_DIR%src\main.c" "%SCRIPT_DIR%src\ui.c" "%SCRIPT_DIR%src\audio.c" "%SCRIPT_DIR%src\network.c" "%SCRIPT_DIR%src\cJSON.c" "%SCRIPT_DIR%src\flac_decoder.c" "%SCRIPT_DIR%src\platform_pc.c" -lwinhttp -lwinmm -lgdi32 -luser32 "-Wl,--subsystem,windows" -o "%SCRIPT_DIR%bin\monochrome_c99_core.exe"

echo ========================================================
echo   [SUCCESS] Executable ready for Host PC:
echo   %SCRIPT_DIR%bin\monochrome_pc.exe
echo   %SCRIPT_DIR%Monochrome.bat
echo ========================================================
