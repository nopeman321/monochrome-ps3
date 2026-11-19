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

echo [1/4] Compiling Cell PPU PowerPC64 ELF binary...
"%ZIG%" cc -target powerpc64-linux-musl -O2 "-Wl,-T,%SCRIPT_DIR%link.ld" "-Wl,-z,norelro" "-Wl,-e,ps3_entry_opd" -o "%SCRIPT_DIR%build\EBOOT.ELF" "%SCRIPT_DIR%src\entry.s" "%SCRIPT_DIR%src\main.c" "%SCRIPT_DIR%src\ui.c" "%SCRIPT_DIR%src\audio.c" "%SCRIPT_DIR%src\network.c" "%SCRIPT_DIR%src\cover.c" "%SCRIPT_DIR%src\cJSON.c" "%SCRIPT_DIR%src\flac_decoder.c" "%SCRIPT_DIR%src\http_ps3.c" "%SCRIPT_DIR%src\platform_ps3.c" "%SCRIPT_DIR%src\lv2.c" -I"%SCRIPT_DIR%include" -I"%SCRIPT_DIR%src"
if errorlevel 1 (
    echo [ERROR] PPU Compilation failed!
    exit /b 1
)

echo [2/4] Patching SCE dynamic import tables (cellGcmSys / cellPad / cellAudio)...
python "%SCRIPT_DIR%tools\sce_imports.py" "%SCRIPT_DIR%build\EBOOT.ELF"
if errorlevel 1 (
    echo [ERROR] SCE import patching failed!
    exit /b 1
)

echo [3/4] Cryptographically signing Free NPDRM SELF (SCE0) with SCETool...
"%SCETOOL%" -0 SELF -1 TRUE -s TRUE -2 04 -3 1010000001000003 -4 01000002 -5 NPDRM -A 0001000000000000 -6 0003004100000000 -b FREE -f UP0001-MCRM00001_00-MONOCHROMEPS3001 -c EXEC -g EBOOT.BIN -e "%SCRIPT_DIR%build\EBOOT.ELF" "%SCRIPT_DIR%pkg_build\USRDIR\EBOOT.BIN"
if errorlevel 1 (
    echo [ERROR] SCETool signing failed!
    exit /b 1
)

echo [4/4] Building installable retail/homebrew PKG for PS3HEN / CFW...
python "%PKG_BUILDER%" "%SCRIPT_DIR%pkg_build" "%SCRIPT_DIR%MCRM00001.pkg"
if errorlevel 1 (
    echo [ERROR] PKG packaging failed!
    exit /b 1
)

echo [DEPLOY] Updating RPCS3 virtual HDD...
if exist "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game" (
    if not exist "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001" mkdir "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001"
    xcopy /E /I /Y "%SCRIPT_DIR%pkg_build" "%SCRIPT_DIR%emulator\rpcs3\dev_hdd0\game\MCRM00001" >nul
    python "%SCRIPT_DIR%tools\configure_rpcs3.py"
)

echo [DIST] Building folder (JB) install package for PS3HEN...
if exist "%SCRIPT_DIR%dist\MCRM00001" rmdir /S /Q "%SCRIPT_DIR%dist\MCRM00001"
mkdir "%SCRIPT_DIR%dist\MCRM00001"
xcopy /E /I /Y "%SCRIPT_DIR%pkg_build" "%SCRIPT_DIR%dist\MCRM00001" >nul
python -c "import shutil; shutil.make_archive(r'%SCRIPT_DIR%dist\Monochrome-PS3-folder', 'zip', r'%SCRIPT_DIR%dist', 'MCRM00001')" 2>nul
if exist "%SCRIPT_DIR%dist\Monochrome-PS3-folder.zip" echo   [OK] Folder package: dist\Monochrome-PS3-folder.zip

echo ========================================================
echo   [SUCCESS] Homebrew PKG ready for PS3HEN:
echo   %SCRIPT_DIR%MCRM00001.pkg
echo   [SUCCESS] Folder (JB) install package ready:
echo   %SCRIPT_DIR%dist\MCRM00001\
echo ========================================================
