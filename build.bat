@echo off
setlocal
rem ===========================================================================
rem  build.bat - compile webrtc_api.dll (x64 MinGW-w64 WebRTC wrapper).
rem
rem  Architecture: x64 only, UCRT runtime (Windows 10+).
rem  libdatachannel + libjuice + usrsctp + libsrtp + libopus are linked
rem  statically; OpenSSL is linked dynamically (libssl-3-x64.dll and
rem  libcrypto-3-x64.dll must sit next to webrtc_api.dll at run time).
rem
rem  Dependencies live in third_party\ next to this script. Override any of
rem  them from the environment, e.g.
rem      set MINI_RTC_DC=C:\src\libdatachannel-0.24.5
rem      build.bat
rem  See DEPENDENCIES.md for how to obtain each dependency.
rem ===========================================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

rem ---- locations (all relative to this script) ------------------------------
if not defined MINI_RTC_DC      set "MINI_RTC_DC=%ROOT%\third_party\libdatachannel-0.24.5"
if not defined MINI_RTC_OPUS    set "MINI_RTC_OPUS=%ROOT%\third_party\opus-mingw64"
if not defined MINI_RTC_OPENSSL set "MINI_RTC_OPENSSL=%ROOT%\third_party\openssl-mingw64\mingw64"

set "DIST=%ROOT%\dist\x64"

rem ---- locate the compiler --------------------------------------------------
rem The compiler is a toolchain rather than a project dependency, so it is not
rem vendored in third_party\. find_gcc.bat looks at MINI_RTC_GCC, then PATH,
rem then a few well-known install locations - no hardcoded machine paths.
call "%ROOT%\find_gcc.bat"
if errorlevel 1 (
  echo.
  echo ERROR: no usable g++.exe.
  echo Install winlibs MinGW-w64 x64 ^(ucrt, posix threads^) and either add it
  echo to PATH or set MINI_RTC_GCC to its mingw64\bin folder.
  endlocal
  exit /b 1
)
set "CXX=%MINI_RTC_GCC%\g++.exe"
echo GCC     = %MINI_RTC_GCC%
echo DC      = %MINI_RTC_DC%
echo OPENSSL = %MINI_RTC_OPENSSL%
echo OPUS    = %MINI_RTC_OPUS%
echo.

rem ---- verify every input before invoking the compiler ---------------------
echo Checking dependencies...
call :need "%CXX%"                                  "g++ - set MINI_RTC_GCC to your winlibs bin"
call :need "%MINI_RTC_DC%\include\rtc\rtc.hpp"      "libdatachannel headers - set MINI_RTC_DC"
call :need "%MINI_RTC_DC%\build\libdatachannel.a"   "libdatachannel.a - run build_libraries.bat"
call :need "%MINI_RTC_DC%\build\deps\libsrtp\libsrtp2.a"  "libsrtp2.a - run build_libraries.bat"
call :need "%MINI_RTC_DC%\build\deps\libjuice\libjuice.a" "libjuice.a - run build_libraries.bat"
call :need "%MINI_RTC_DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a" "libusrsctp.a - run build_libraries.bat"
call :need "%MINI_RTC_OPUS%\include\opus\opus.h"    "opus headers - set MINI_RTC_OPUS"
call :need "%MINI_RTC_OPUS%\lib\libopus.a"          "libopus.a - run build_libraries.bat"
call :need "%MINI_RTC_OPENSSL%\lib\libssl.dll.a"    "libssl.dll.a - run build_libraries.bat"
call :need "%MINI_RTC_OPENSSL%\lib\libcrypto.dll.a" "libcrypto.dll.a - run build_libraries.bat"

if defined FAILED (
  echo.
  echo ERROR: build dependencies are missing - see the MISSING lines above.
  echo Run build_libraries.bat to download and prepare them.
  endlocal
  exit /b 1
)

echo.
echo Compiling webrtc_api.dll - static CRT/libstdc++/libopus/libdatachannel, dynamic OpenSSL

if not exist "%DIST%" mkdir "%DIST%"

"%CXX%" -shared -O2 -static -DNDEBUG -Wall -Wextra ^
  -Wl,--no-insert-timestamp ^
  -o "%DIST%\webrtc_api.dll" "%ROOT%\webrtc_api.cpp" ^
  -I"%MINI_RTC_DC%\include" -I"%MINI_RTC_OPUS%\include" ^
  "%MINI_RTC_DC%\build\libdatachannel.a" ^
  "%MINI_RTC_DC%\build\deps\libsrtp\libsrtp2.a" ^
  "%MINI_RTC_DC%\build\deps\libjuice\libjuice.a" ^
  "%MINI_RTC_DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a" ^
  "%MINI_RTC_OPUS%\lib\libopus.a" ^
  "%MINI_RTC_OPENSSL%\lib\libssl.dll.a" "%MINI_RTC_OPENSSL%\lib\libcrypto.dll.a" ^
  -lws2_32 -lole32 -liphlpapi -lbcrypt -lcrypt32 -luuid

if errorlevel 1 (
  echo.
  echo BUILD FAILED
  endlocal
  exit /b 1
)

rem ---- deploy ---------------------------------------------------------------
rem The OpenSSL runtime DLLs must sit next to webrtc_api.dll. They are not
rem rebuilt here: dist\x64 already carries the pinned 3.6.3 pair.
for %%D in (libssl-3-x64.dll libcrypto-3-x64.dll) do (
  if not exist "%DIST%\%%D" (
    if exist "%MINI_RTC_OPENSSL%\bin\%%D" (
      copy /y "%MINI_RTC_OPENSSL%\bin\%%D" "%DIST%\%%D%" >nul
    ) else (
      echo WARNING: %%D is missing from dist\x64 and was not found in %MINI_RTC_OPENSSL%\bin
    )
  )
)

rem keep the legacy top-level copy in sync with the deployed build
copy /y "%DIST%\webrtc_api.dll" "%ROOT%\webrtc_api.dll" >nul

echo.
echo BUILD OK - deployed to dist\x64:
dir /b "%DIST%"
endlocal
exit /b 0

:need
if not exist "%~1" (
  echo   MISSING: %~2
  set "FAILED=1"
)
exit /b 0
