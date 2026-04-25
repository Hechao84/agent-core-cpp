@echo off
REM build_third_party.bat - Build third-party libraries for Windows
REM Downloads nlohmann/json headers and SQLite3 source, builds sqlite3.dll

setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

echo ==========================================
echo  Building Third-Party Libraries for Windows
echo ==========================================

set "TP_DIR=%SCRIPT_DIR%\third_party"
set "LIBS_DIR=%SCRIPT_DIR%\libs"
set "TP_INCLUDE=%TP_DIR%\include"
set "TP_SRC=%TP_DIR%\src"

mkdir "%TP_DIR%" 2>nul
mkdir "%TP_INCLUDE%" 2>nul
mkdir "%TP_SRC%" 2>nul
mkdir "%LIBS_DIR%" 2>nul

REM [1/2] nlohmann/json (header-only)
if exist "%TP_INCLUDE%\nlohmann\json.hpp" (
    echo [1/2] nlohmann/json already downloaded, ready.
) else (
    echo [1/2] Downloading nlohmann/json v3.11.3...
    
    if exist "%TP_DIR%\json-3.11.3" (
        rmdir /s /q "%TP_DIR%\json-3.11.3"
    )

    REM Try cloning from GitHub
    git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git "%TP_DIR%\json-3.11.3"
    if errorlevel 1 (
        echo ERROR: Failed to download nlohmann/json v3.11.3
        echo Please ensure git is installed and network is available.
        exit /b 1
    )
    
    mkdir "%TP_INCLUDE%\nlohmann" 2>nul
    copy "%TP_DIR%\json-3.11.3\include\nlohmann\json.hpp" "%TP_INCLUDE%\nlohmann\" >nul
    copy "%TP_DIR%\json-3.11.3\include\nlohmann\json_fwd.hpp" "%TP_INCLUDE%\nlohmann\" >nul
    
    rmdir /s /q "%TP_DIR%\json-3.11.3"
    echo nlohmann/json headers installed.
)

REM [2/2] SQLite3 (build DLL)
if exist "%LIBS_DIR%\sqlite3.dll" (
    echo [2/2] sqlite3.dll already built, ready.
) else (
    echo [2/2] Building SQLite3...
    
    if not exist "%TP_SRC%\sqlite3-src" (
        echo Downloading SQLite3 amalgamation...
        mkdir "%TP_SRC%\sqlite3-src" 2>nul
        
        powershell -Command "Invoke-WebRequest -Uri 'https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip' -OutFile '%TP_SRC%\sqlite3.zip'"
        if errorlevel 1 (
            echo ERROR: Failed to download SQLite3
            exit /b 1
        )
        
        powershell -Command "Expand-Archive -Path '%TP_SRC%\sqlite3.zip' -DestinationPath '%TP_SRC%' -Force"
        
        for /d %%D in ("%TP_SRC%\sqlite-amalgamation-*") do (
            move "%%D\*.*" "%TP_SRC%\sqlite3-src\" >nul
            rmdir /s /q "%%D"
        )
        del "%TP_SRC%\sqlite3.zip"
    )
    
    REM Copy sqlite3.h to include
    copy "%TP_SRC%\sqlite3-src\sqlite3.h" "%TP_INCLUDE%\" >nul
    copy "%TP_SRC%\sqlite3-src\sqlite3ext.h" "%TP_INCLUDE%\" >nul
    
    REM Check if Visual Studio tools are available
    where cl >nul 2>&1
    if errorlevel 1 (
        echo ERROR: cl.exe not found. Please run this from a "Developer Command Prompt for VS".
        echo Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
        exit /b 1
    )
    
    echo Compiling sqlite3.dll...
    cl /LD /O2 /DSQLITE_API=__declspec^(dllexport^) /Fe"%LIBS_DIR%\sqlite3.dll" "%TP_SRC%\sqlite3-src\sqlite3.c"
    
    if errorlevel 1 (
        echo ERROR: Failed to compile sqlite3.dll
        exit /b 1
    )
    
    if exist "%LIBS_DIR%\sqlite3.exp" (
        del "%LIBS_DIR%\sqlite3.exp" >nul 2>nul
    )
    if exist "%LIBS_DIR%\sqlite3.obj" (
        del "%LIBS_DIR%\sqlite3.obj" >nul 2>nul
    )
    
    echo sqlite3.dll built successfully.
)

echo.
echo ==========================================
echo  Third-Party Libraries Ready
echo ==========================================
echo Headers: %TP_INCLUDE%
echo Libs:    %LIBS_DIR%
REM Explicitly succeed to avoid false failures from mkdir/echo
exit /b 0
