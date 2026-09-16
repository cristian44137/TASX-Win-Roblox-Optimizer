@echo off
rem Runs the separate console/debug build with a visible live log.
rem Build it once with: mingw32-make debug
cd /d "%~dp0"
chcp 65001 >nul
if not exist "TASX-debug.exe" (
    echo TASX-debug.exe not found.
    echo Build it first with: mingw32-make debug
    pause
    exit /b 1
)
echo TASX visible debug agent. Close this window or press Ctrl+C to stop it.
TASX-debug.exe
pause
