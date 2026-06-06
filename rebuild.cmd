@echo off
chcp 65001 >nul
title Rebuild QR Blocker Plugin

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

if errorlevel 1 (
    echo [ERROR] Failed to initialize VS environment
    pause
    exit /b 1
)

set BUILD_DIR=e:\obs\qr-blocker-plugin\build
set SRC_DIR=e:\obs\qr-blocker-plugin
set OBS_DIR=C:\Program Files\obs-studio
set OBS_DLL=%OBS_DIR%\bin\64bit\obs.dll
set OBS_LIB=%OBS_DIR%\bin\64bit\obs.lib

:: Clean build dir
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

:: ============================================================
:: Step 1: Generate OBS Import Library
:: ============================================================

echo.
echo [Step 1/2] Generating OBS import library...

if not exist "%OBS_DLL%" (
    echo [ERROR] OBS DLL not found at: %OBS_DLL%
    pause
    exit /b 1
)

if not exist "%OBS_LIB%" (
    echo [INFO] Generating obs.lib from obs.dll exports...

    dumpbin /exports "%OBS_DLL%" > "%BUILD_DIR%\obs_exports.txt"

    powershell -NoProfile -Command ^
        "$txt = Get-Content '%BUILD_DIR%\obs_exports.txt';" ^
        "$exports = @();" ^
        "foreach ($l in $txt) {" ^
        "   if ($l -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)\s*=') {" ^
        "       $exports += $matches[1];" ^
        "   }" ^
        "}" ^
        "'EXPORTS' | Out-File '%BUILD_DIR%\obs.def' -Encoding ASCII;" ^
        "foreach ($e in $exports) { '    ' + $e | Out-File '%BUILD_DIR%\obs.def' -Encoding ASCII -Append };" ^
        "Write-Host ('Found ' + $exports.Count + ' exports')"

    lib /def:"%BUILD_DIR%\obs.def" /out:"%BUILD_DIR%\obs.lib" /machine:x64

    if not errorlevel 1 (
        set OBS_LIB=%BUILD_DIR%\obs.lib
        echo [OK] Import library generated: %OBS_LIB%
    ) else (
        echo [ERROR] Failed to generate import library.
        pause
        exit /b 1
    )
) else (
    echo [OK] Import library already exists.
)

:: ============================================================
:: Step 2: Compile Plugin
:: ============================================================

echo.
echo [Step 2/2] Compiling plugin...

cd /d "%SRC_DIR%"

cl /nologo /O2 /MD /LD ^
    /I"%SRC_DIR%\deps\obs-headers" ^
    /I"%SRC_DIR%\deps\obs-headers\graphics" ^
    /I"%SRC_DIR%\deps\obs-headers\util" ^
    /I"%SRC_DIR%\deps\obs-headers\media-io" ^
    /I"%SRC_DIR%\deps\obs-headers\callback" ^
    /I"%SRC_DIR%\deps\quirc\lib" ^
    /Fe"%BUILD_DIR%\qr-blocker-filter.dll" ^
    "%SRC_DIR%\src\qr-blocker-filter.c" ^
    "%SRC_DIR%\deps\quirc\lib\quirc.c" ^
    "%SRC_DIR%\deps\quirc\lib\identify.c" ^
    "%SRC_DIR%\deps\quirc\lib\version_db.c" ^
    /link "%OBS_LIB%" gdi32.lib user32.lib ole32.lib winmm.lib

if errorlevel 1 (
    echo [ERROR] Compile failed.
    pause
    exit /b 1
)

:: ============================================================
:: Step 3: Install to OBS
:: ============================================================

echo.
echo Installing plugin to OBS...

copy /y "%BUILD_DIR%\qr-blocker-filter.dll" "%OBS_DIR%\obs-plugins\64bit\qr-blocker-filter.dll"

if errorlevel 0 (
    echo [OK] Installed: %OBS_DIR%\obs-plugins\64bit\qr-blocker-filter.dll
) else (
    echo [WARN] Copy failed. Try running as Administrator.
)

:: Install locale
set OBS_DATA_DIR=%OBS_DIR%\data\obs-plugins\qr-blocker-filter
if not exist "%OBS_DATA_DIR%" mkdir "%OBS_DATA_DIR%"
xcopy /E /Y "%SRC_DIR%\data\*" "%OBS_DATA_DIR%\" >nul 2>&1
echo [OK] Locale files installed.

echo.
echo ========================================
echo  BUILD COMPLETE!
echo ========================================
echo.
echo  Output: %BUILD_DIR%\qr-blocker-filter.dll
echo  OBS:    %OBS_DIR%\obs-plugins\64bit\qr-blocker-filter.dll
echo.
pause
