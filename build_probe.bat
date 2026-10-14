@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
call "%ROOT%\find_gcc.bat"
if errorlevel 1 exit /b 1
set "CXX=%MINI_RTC_GCC%\g++.exe"
"%CXX%" -std=c++17 -O2 -o examples\mfprobe.exe examples\mfprobe.cpp -lole32 -lmfplat -lmf -lmfuuid -luuid 2>&1
if errorlevel 1 (echo PROBE BUILD FAILED) else (echo PROBE BUILD OK)