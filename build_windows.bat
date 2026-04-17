@echo off
REM build_windows.bat - Build jiuwen-lite project on Windows
REM
REM Prerequisites:
REM   - Visual Studio 2019 or later (with C++ desktop development workload)
REM   - CMake (available in PATH)
REM   - libcurl (must be installed and available)
REM
REM Run this script from a "Developer Command Prompt for VS" or after running VsDevCmd.bat

setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

echo ==========================================
echo  jiuwen-lite Windows Build
echo ==========================================

REM Check for Visual Studio developer tools
where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found.
    echo Please run this from a "Developer Command Prompt for VS" ^
         or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    exit /b 1
)

REM Check for CMake
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake.exe not found. Please install CMake and add it to PATH.
    exit /b 1
)

REM ============================================================
REM Step 1: Build third-party libraries
REM ============================================================
echo.
echo ==========================================
echo  [1/3] Building Third-Party Libraries
echo ==========================================

call "%SCRIPT_DIR%\build_third_party.bat"
if errorlevel 1 (
    echo ERROR: Third-party build failed.
    exit /b 1
)

echo.

REM ============================================================
REM Step 2: Configure Main Project
REM ============================================================
echo ==========================================
echo  [2/3] Configuring Main Project
echo ==========================================

set "BUILD_DIR=%SCRIPT_DIR%\build-windows"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo --- Running CMake...

REM Auto-detect vcpkg toolchain if VCPKG_ROOT is set
set "VP_TOOLCHAIN="
if defined VCPKG_ROOT (
    set "VP_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
    echo Using vcpkg toolchain from: %VCPKG_ROOT%
) else if defined VCPKG_INSTALLATION_ROOT (
    set "VP_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_INSTALLATION_ROOT%/scripts/buildsystems/vcpkg.cmake"
    echo Using vcpkg toolchain from: %VCPKG_INSTALLATION_ROOT%
)

cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release ^
    %VP_TOOLCHAIN%

if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    echo NOTE: If you have Visual Studio 2019, change the generator to "Visual Studio 16 2019"
    exit /b 1
)

echo --- CMake configuration complete.
echo.

REM ============================================================
REM Step 3: Build Main Project
REM ============================================================
echo ==========================================
echo  [3/3] Building Main Project
echo ==========================================

echo --- Compiling...

cmake --build . --target jiuwenClaw --config Release -- /nologo /verbosity:minimal

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

REM ============================================================
REM Package output
REM ============================================================
echo.
echo --- Packaging output...

if not exist "%SCRIPT_DIR%\dist\windows" mkdir "%SCRIPT_DIR%\dist\windows"

copy "%BUILD_DIR%\Release\jiuwenClaw.exe" "%SCRIPT_DIR%\dist\windows\" >nul
copy "%BUILD_DIR%\Release\agent_framework.dll" "%SCRIPT_DIR%\dist\windows\" >nul 2>nul
if exist "%BUILD_DIR%\Release\agent_framework.lib" (
    copy "%BUILD_DIR%\Release\agent_framework.lib" "%SCRIPT_DIR%\dist\windows\" >nul
)

REM Copy SQLite DLL if it exists
if exist "%SCRIPT_DIR%\libs\sqlite3.dll" (
    copy "%SCRIPT_DIR%\libs\sqlite3.dll" "%SCRIPT_DIR%\dist\windows\" >nul
)

REM Copy skills directory if exists
if exist "%SCRIPT_DIR%\my_skills" (
    xcopy /E /I /Y "%SCRIPT_DIR%\my_skills" "%SCRIPT_DIR%\dist\windows\my_skills\" >nul
    echo Skills copied to dist\windows\my_skills
)

echo.
echo ==========================================
echo  Build Complete!
echo ==========================================
echo Library: dist\windows\agent_framework.dll
dir "%SCRIPT_DIR%\dist\windows\agent_framework.dll" 2>nul | find "agent_framework"
echo Binary:  dist\windows\jiuwenClaw.exe
dir "%SCRIPT_DIR%\dist\windows\jiuwenClaw.exe" 2>nul | find "jiuwenClaw"
echo.
echo To run the demo app:
echo   cd %SCRIPT_DIR%
echo   set PATH=%SCRIPT_DIR%\dist\windows;%%PATH%%
echo   dist\windows\jiuwenClaw.exe

endlocal
