@echo off
rem TASX build script (no make needed).
rem   build.bat          -> console build with logs: TASX.exe
rem   build.bat silent   -> headless background build: TASX.exe
rem Uses MSYS2 UCRT64 toolchain; edit TOOLCHAIN below if it lives elsewhere.

set "TOOLCHAIN=D:\msys2\ucrt64\bin"
set "PATH=%TOOLCHAIN%;%PATH%"

where g++ >nul 2>&1
if errorlevel 1 (
    echo [build] g++ not found in %TOOLCHAIN%
    pause
    exit /b 1
)

cd /d "%~dp0"

set CXXFLAGS=-std=c++23 -O2 -m64 -static -Wall -Wextra
set CFLAGS=-std=c11 -O2 -m64 -Wall -Wextra
set LIBS=-lwbemuuid -lpsapi -lpowrprof -luser32 -lkernel32 -lole32 -loleaut32 -luuid -lcomctl32 -ladvapi32 -liphlpapi
set OBJS=master.o CPU.o WMI.o trimmer.o metrics.o winhook.o hotkey.o config.o ntsys.o tweaks.o fflags.o jobs.o stats.o audio.o warm.o desktop.o netcache.o respawn.o tasx.res
if /i "%~1"=="silent" (set "MODE=-DTASX_GUI") else (set "MODE=-DTASX_CONSOLE")

echo [build] compiling resources...
windres tasx.rc -O coff -o tasx.res || goto :fail

echo [build] compiling C core...
gcc %CFLAGS% -c Infra\config.c -o config.o   || goto :fail
gcc %CFLAGS% -c Infra\ntsys.c  -o ntsys.o    || goto :fail

echo [build] compiling C++ modules...
g++ %CXXFLAGS% %MODE% -c Infra\master.cpp  -o master.o   || goto :fail
g++ %CXXFLAGS% -c Infra\CPU.cc      -o CPU.o      || goto :fail
g++ %CXXFLAGS% -c Infra\WMI.cc      -o WMI.o      || goto :fail
g++ %CXXFLAGS% -c Infra\trimmer.cc  -o trimmer.o  || goto :fail
g++ %CXXFLAGS% -c Infra\metrics.cc  -o metrics.o  || goto :fail
g++ %CXXFLAGS% -c Infra\winhook.cc  -o winhook.o  || goto :fail
g++ %CXXFLAGS% -c Infra\hotkey.cc   -o hotkey.o   || goto :fail
g++ %CXXFLAGS% -c Infra\tweaks.cc   -o tweaks.o   || goto :fail
g++ %CXXFLAGS% -c Infra\fflags.cc   -o fflags.o   || goto :fail
g++ %CXXFLAGS% -c Infra\jobs.cc     -o jobs.o     || goto :fail
g++ %CXXFLAGS% -c Infra\stats.cc    -o stats.o    || goto :fail
g++ %CXXFLAGS% -c Infra\audio.cc    -o audio.o    || goto :fail
g++ %CXXFLAGS% -c Infra\warm.cc     -o warm.o     || goto :fail
g++ %CXXFLAGS% -c Infra\desktop.cc  -o desktop.o  || goto :fail
g++ %CXXFLAGS% -c Infra\netcache.cc -o netcache.o || goto :fail
g++ %CXXFLAGS% -c Infra\respawn.cc -o respawn.o || goto :fail

if /i "%~1"=="silent" (
    echo [build] linking headless background build...
    g++ %CXXFLAGS% -mwindows %OBJS% -o TASX.exe %LIBS% || goto :fail
) else (
    echo [build] linking console build...
    g++ %CXXFLAGS% %OBJS% -o TASX.exe %LIBS% || goto :fail
)

del /q *.o tasx.res >nul 2>&1
echo [build] OK: TASX.exe created
pause
exit /b 0

:fail
echo [build] FAILED
pause
exit /b 1
