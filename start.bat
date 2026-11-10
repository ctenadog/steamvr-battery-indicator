@echo off
REM One-click launcher for SteamVR Battery Indicator (Windows).
REM Double-click this file to start. Builds on first run if a compiler is present.
setlocal
cd /d "%~dp0"

set "BIN=steamvr_battery_indicator.exe"
if not exist "%BIN%" goto build
goto run

:build
where g++ >nul 2>&1
if errorlevel 1 (
    echo A C++ compiler ^(g++^) was not found, so the program cannot be built.
    echo Install MinGW-w64 or build with CMake/MSVC, then run this file again.
    pause
    exit /b 1
)
echo Building with g++ ...
g++ -std=c++17 -O2 -Isrc src\main.cpp -o "%BIN%"
if errorlevel 1 (
    echo Build failed. Press any key to close.
    pause >nul
    exit /b 1
)

:run
"%BIN%" %*
if errorlevel 1 (
    echo.
    echo The indicator exited with an error. Press any key to close.
    pause >nul
)
endlocal

