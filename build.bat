@echo off
rem Build webrtc_api.dll (GameMaker WebRTC wrapper) from libdatachannel source.
rem This script no longer contains hard-coded absolute paths. Instead it:
rem  - accepts environment variables: GCC (mingw64 bin folder), DC (libdatachannel root), OPENSSL (mingw64 OpenSSL root)
rem  - tries to find g++.exe on PATH if GCC isn't set
rem  - falls back to relative locations next to this script if available
setlocal

rem -------------------------
rem Configuration (override by setting env vars before running):
rem   set GCC=C:\path\to\mingw64\bin
rem   set DC=C:\path\to\libdatachannel
rem   set OPENSSL=C:\path\to\openssl-mingw64\mingw64
rem -------------------------

rem Try to find GCC (g++.exe) if not set
if "%GCC%"=="" (
  for /f "delims=" %%I in ('where g++.exe 2^>nul') do (
    set "GCC=%%~dpI"
    goto :found_gcc
  )
  rem Try common relative locations (next to this script)
  if exist "%~dp0winlibs\mingw64\bin\g++.exe" (
    set "GCC=%~dp0winlibs\mingw64\bin"
    goto :found_gcc
  )
  echo ERROR: g++.exe not found on PATH and GCC not set.
  echo Please install a MinGW-w64 toolchain, add g++.exe to PATH, or set the GCC environment variable to the mingw64 bin folder.
  goto :fail
)
:found_gcc

rem Try to set libdatachannel root (DC) to a sensible default if not set
if "%DC%"=="" (
  if exist "%~dp0..\libdatachannel\build\libdatachannel-static.a" (
    set "DC=%~dp0..\libdatachannel"
  ) else if exist "%USERPROFILE%\Documents\libdatachannel\build\libdatachannel-static.a" (
    set "DC=%USERPROFILE%\Documents\libdatachannel"
  ) else (
    echo WARNING: libdatachannel root not found automatically. Set DC environment variable to the libdatachannel root (where build\libdatachannel-static.a lives) if the build fails.
    set "DC=%~dp0..\libdatachannel"
  )
)

rem Try to set OpenSSL (mingw) root if not set
if "%OPENSSL%"=="" (
  if exist "%~dp0openssl-mingw64\mingw64\lib\libssl.dll.a" (
    set "OPENSSL=%~dp0openssl-mingw64\mingw64"
  ) else if exist "%USERPROFILE%\Documents\openssl-mingw64\mingw64\lib\libssl.dll.a" (
    set "OPENSSL=%USERPROFILE%\Documents\openssl-mingw64\mingw64"
  ) else (
    echo WARNING: OpenSSL (mingw) not found automatically. Set OPENSSL environment variable to the mingw64 OpenSSL root (contains lib\libssl.dll.a) if the build fails.
    set "OPENSSL=%~dp0openssl-mingw64\mingw64"
  )
)

rem Show what we will use
echo Using GCC=%GCC%
echo Using DC=%DC%
echo Using OPENSSL=%OPENSSL%

set "INC=%DC%\include;%DC%\build"

echo Compiling wrapper DLL (static CRT, dynamic OpenSSL) ...
"%GCC%\g++.exe" -shared -O2 -static -o webrtc_api.dll webrtc_api.cpp -I"%DC%\include" -I"%DC%\build" ^
  "%DC%\build\libdatachannel-static.a" ^
  "%DC%\build\deps\libjuice\libjuice-static.a" ^
  "%DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a" ^
  "%OPENSSL%\lib\libssl.dll.a" "%OPENSSL%\lib\libcrypto.dll.a" ^
  -lws2_32 -lole32 -liphlpapi -lbcrypt -lcrypt32
if errorlevel 1 goto :fail
echo BUILD OK - deploy webrtc_api.dll + libssl-3-x64.dll + libcrypto-3-x64.dll
goto :eof
:fail
echo BUILD FAILED
endlocal
