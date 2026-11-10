@echo off
REM One-click launcher for SteamVR Battery Indicator (Windows).
REM Double-click this file to start.
REM
REM The launcher prefers the prebuilt binary in dist\ (no compiler needed).
REM If it is missing, it falls back to building from source with g++.

setlocal
cd /d "%~dp0"

set "BIN=dist\steamvr_battery_indicator.exe"
if exist "%BIN%" goto run

REM No prebuilt binary: try to build one.
where g++ >nul 2>&1
if errorlevel 1 goto nocompiler

echo No prebuilt binary found. Building from source with g++ ...
g++ -std=c++17 -O2 -Isrc src\main.cpp -o "%BIN%"
if errorlevel 1 (
    echo Build failed. Press any key to close.
    pause >nul
    exit /b 1
)
goto run

:nocompiler
echo.
echo No prebuilt binary was found in dist\ and no C++ compiler (g++) is installed.
echo.
echo Options:
echo   1. Get the prebuilt dist\steamvr_battery_indicator.exe from the repository
echo      or the GitHub releases, place it in dist\, then run this file again.
echo   2. Install MinGW-w64 (https://www.mingw-w64.org/) or build with
echo      CMake/MSVC, then run this file again.
echo.
pause
exit /b 1

:run
"%BIN%" %*
REM A crash (e.g. 0xC0000005) reports a negative code, for which "errorlevel 1"
REM is false; compare against 0 explicitly so the window always stays open.
if not "%errorlevel%"=="0" (
    echo.
    echo The indicator exited with code %errorlevel%. See steamvr_battery_indicator.log
    echo in this folder for details. Press any key to close.
    pause >nul
) else (
    echo.
    echo The indicator closed. Press any key to close.
    pause >nul
)
endlocal

