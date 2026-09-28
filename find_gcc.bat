@echo off
rem ===========================================================================
rem  find_gcc.bat - locate an x64 MinGW-w64 g++ and publish MINI_RTC_GCC.
rem
rem  Called by build.bat and build_libraries.bat. Deliberately has NO
rem  setlocal: it must publish MINI_RTC_GCC into the caller's environment.
rem
rem  Resolution order:
rem    1. MINI_RTC_GCC, if it already points at a usable g++.exe
rem    2. third_party\toolchain\mingw64\bin   (the vendored toolchain)
rem    3. g++ on PATH
rem
rem  No common install directories are searched. The toolchain is vendored so
rem  the build is reproducible and does not depend on whatever happens to be
rem  installed on the machine.
rem
rem  A ucrt + posix-threads build is required (that is what webrtc_api.dll
rem  targets). Exits 1 if nothing usable is found.
rem ===========================================================================

if not defined ROOT set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if defined MINI_RTC_GCC if exist "%MINI_RTC_GCC%\g++.exe" goto :report

rem --- vendored toolchain ----------------------------------------------------
if exist "%ROOT%\third_party\toolchain\mingw64\bin\g++.exe" (
  set "MINI_RTC_GCC=%ROOT%\third_party\toolchain\mingw64\bin"
  goto :report
)

rem --- g++ on PATH -----------------------------------------------------------
set "_gpp="
for /f "delims=" %%G in ('where g++ 2^>nul') do if not defined _gpp set "_gpp=%%G"
if defined _gpp (
  for %%I in ("%_gpp%") do set "MINI_RTC_GCC=%%~dpI"
  goto :report
)

exit /b 1

rem ---------------------------------------------------------------------------
:report
rem A 32-bit toolchain cannot produce this DLL. Say so now rather than letting
rem the user hit a wall of confusing linker errors later.
set "_triple="
for /f "delims=" %%T in ('"%MINI_RTC_GCC%\g++.exe" -dumpmachine 2^>nul') do if not defined _triple set "_triple=%%T"
if not defined _triple (
  echo ERROR: could not run "%MINI_RTC_GCC%\g++.exe" -dumpmachine
  exit /b 1
)
if not "%_triple%"=="x86_64-w64-mingw32" (
  echo WARNING: "%_triple%" is not an x86_64-w64-mingw32 toolchain.
  echo          webrtc_api.dll is x64 only.
  exit /b 1
)
echo   using %_triple%  [%MINI_RTC_GCC%]
rem Informational: a ucrt build ships libucrt.a, an msvcrt build does not. Both
rem live in the target libdir, not necessarily in mingw64\lib.
if not exist "%MINI_RTC_GCC%\..\x86_64-w64-mingw32\lib\libucrt.a" (
  if not exist "%MINI_RTC_GCC%\..\lib\libucrt.a" (
    echo   note: no libucrt.a found - this looks like an msvcrt build, not ucrt
  )
)
exit /b 0
