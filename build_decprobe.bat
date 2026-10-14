@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
call "%ROOT%\find_gcc.bat"
if errorlevel 1 exit /b 1
set "CXX=%MINI_RTC_GCC%\g++.exe"
"%CXX%" -std=c++17 -O2 -o examples\mfdecprobe.exe examples\mfdecprobe.cpp -lole32 -lmfplat -lmf -lmfuuid -luuid -ld3d11 -ldxgi 2>&1
if errorlevel 1 (echo BUILD FAILED) else (echo BUILD OK)