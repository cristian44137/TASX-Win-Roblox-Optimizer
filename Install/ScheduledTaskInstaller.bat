@echo off

:: Prompt for UAC (Administrator)
:: We need this because TASX's task requires Administrator to both carry out its functionality and embed itself in startup
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator elevation required. Prompting...
    set "params=%*"
    echo Set UAC = CreateObject^("Shell.Application"^) > "%temp%\uac.vbs"
    echo UAC.ShellExecute "cmd.exe", "/c ""%~s0"" %params%", "", "runas", 1 >> "%temp%\uac.vbs"
    "%temp%\uac.vbs"
    del "%temp%\uac.vbs"
    exit /b
)

:: Name = TASX_Agent
set TASKNAME=TASX_Agent
:: The installer lives in Install\ while the shipped binary is one level up.
set "EXEPATH=%~dp0..\TASX.exe"
for %%I in ("%EXEPATH%") do set "EXEPATH=%%~fI"
if not exist "%EXEPATH%" (
    echo TASX : executable not found:
    echo         "%EXEPATH%"
    echo Put TASX.exe in the project/release root and run this installer again.
    pause
    exit /b 1
)

:: Check if task already exists
schtasks /query /tn "%TASKNAME%" >nul 2>&1
if %errorlevel% equ 0 (
    echo TASX : '%TASKNAME%' already exists. Skipping installation.
    echo If you wish to reinstall it, run Uninstall.bat first.
    pause
    exit /b
)

:: Create elevated scheduled task
:: Elevation is required since TASX modifies other processes' Windows configuration settings
schtasks /create ^
  /tn "%TASKNAME%" ^
  /f ^
  /rl highest ^
  /sc onlogon ^
  /tr "\"%EXEPATH%\"" ^
  /ru "%USERNAME%"

if errorlevel 1 (
    echo TASX : failed to create scheduled task. No partial installation is kept.
    schtasks /delete /tn "%TASKNAME%" /f >nul 2>&1
    exit /b 1
)

cls
echo TASX has been successfully installed.
echo.
echo TASX v0.3 BETA : Made by @8damon
echo TASX will run silently in the background and start with your computer.
echo To uninstall, run Uninstall.bat
echo For help, join https://hub.ryftenius.com/
pause
