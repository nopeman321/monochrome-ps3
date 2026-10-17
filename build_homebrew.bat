@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   MONOCHROME PS3 - Homebrew Build System (PS3HEN / CFW)
echo ========================================================

set SCRIPT_DIR=%~dp0
set ZIG=%SCRIPT_DIR%tools\zig\zig.exe
set SCETOOL=%SCRIPT_DIR%tools\scetool\scetool.exe
set PS3=%SCRIPT_DIR%tools\scetool\data
set PKG_BUILDER=%SCRIPT_DIR%tools\pkg_builder.py

if not exist "%SCRIPT_DIR%build" mkdir "%SCRIPT_DIR%build"
if not exist "%SCRIPT_DIR%pkg_build\USRDIR" mkdir "%SCRIPT_DIR%pkg_build\USRDIR"

echo [1/3] Compiling Cell PPU PowerPC64 ELF binary...
"%ZIG%" cc -target powerpc64-linux-musl -O2 "-Wl,-T,%SCRIPT_DIR%link.ld" "-Wl,-z,norelro" -o "%SCRIPT_DIR%build\EBOOT.ELF" "%SCRIPT_DIR%src\main.c" "%SCRIPT_DIR%src\ui.c" "%SCRIPT_DIR%src\audio.c" "%SCRIPT_DIR%src\network.c" "%SCRIPT_DIR%src\cJSON.c" "%SCRIPT_DIR%src\lv2.c" -I"%SCRIPT_DIR%include" -I"%SCRIPT_DIR%src"
if errorlevel 1 (
    echo [ERROR] PPU Compilation failed!
    exit /b 1
)

echo [2/3] Cryptographically signing Free NPDRM SELF (SCE0) with SCETool...
"%SCETOOL%" -0 SELF -1 TRUE -s TRUE -2 04 -3 1010000001000003 -4 01000002 -5 NPDRM -A 0001000000000000 -6 0003004100000000 -b FREE -f UP0001-MCRM00001_00-MONOCHROMEPS3001 -c EXEC -g EBOOT.BIN -e "%SCRIPT_DIR%build\EBOOT.ELF" "%SCRIPT_DIR%pkg_build\USRDIR\EBOOT.BIN"
if errorlevel 1 (
    echo [ERROR] SCETool signing failed!
    exit /b 1
)

echo [3/3] Building installable retail/homebrew PKG for PS3HEN / CFW...
python "%PKG_BUILDER%" "%SCRIPT_DIR%pkg_build" "%SCRIPT_DIR%MCRM00001.pkg"
if errorlevel 1 (
    echo [ERROR] PKG packaging failed!
    exit /b 1
)

echo [DEPLOY] Updating RPCS3 virtual HDD...
if exist "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001\USRDIR" (
    copy /y "%SCRIPT_DIR%pkg_build\USRDIR\EBOOT.BIN" "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001\USRDIR\EBOOT.BIN" >nul
)

echo ========================================================
echo   [SUCCESS] Homebrew PKG ready for PS3HEN:
echo   %SCRIPT_DIR%MCRM00001.pkg
echo ========================================================
