@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  build_libraries.bat - fetch and prepare every third-party dependency
rem  required to build webrtc_api.dll.
rem
rem  Run this once, then run build.bat. Nothing here builds webrtc_api.dll.
rem
rem  All paths are relative to this script; nothing is hardcoded to a
rem  particular machine. Installs into third_party\ (not tracked by git):
rem    third_party\openssl-mingw64\mingw64   OpenSSL headers + import libs
rem    third_party\opus-mingw64               libopus.a + headers
rem    third_party\libdatachannel-0.24.5     libdatachannel.a + deps
rem
rem  Requires on PATH: git, cmake, tar, curl, certutil - and an x64
rem  MinGW-w64 g++ (ucrt, posix threads). find_gcc.bat locates the compiler;
rem  set MINI_RTC_GCC to override it.
rem
rem  Other overrides:
rem    set MINI_RTC_FORCE=1    re-download/rebuild even if already present
rem
rem  See DEPENDENCIES.md for background and troubleshooting.
rem ===========================================================================

rem ---- pinned versions ------------------------------------------------------
set "DC_VERSION=0.24.5"
set "DC_TAG=v0.24.5"
set "OPUS_VERSION=1.5.2"
set "OSSL_VERSION=3.6.4-1"

set "DC_REPO=https://github.com/paullouisageneau/libdatachannel.git"
set "OPUS_URL=https://downloads.xiph.org/releases/opus/opus-%OPUS_VERSION%.tar.gz"
set "OSSL_URL=https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-openssl-%OSSL_VERSION%-any.pkg.tar.zst"

rem Upstream checksums, verified when this script was written. The MSYS2
rem mirror deletes superseded package versions, so if the download 404s, take
rem the current URL/sha from
rem   https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-openssl
rem and edit OSSL_URL / OSSL_SHA below.
set "OPUS_SHA=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
set "OSSL_SHA=0b7b6497910450d9c9c0feef66f00493dc54f4afeab1e1144447e8f9d6e4366f"

rem ---- layout (must match the defaults in build.bat) ------------------------
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "TP=%ROOT%\third_party"
set "CACHE=%TP%\_downloads"
set "OPENSSL_PREFIX=%TP%\openssl-mingw64\mingw64"
set "OPUS_PREFIX=%TP%\opus-mingw64"
set "DC=%TP%\libdatachannel-%DC_VERSION%"
set "FORCE=%MINI_RTC_FORCE%"

echo.
echo === mini-rtc dependency setup ===
echo project : %ROOT%
echo.

rem ---- preflight ------------------------------------------------------------
echo.
call :step "Checking required tools"
for %%T in (git cmake tar curl certutil) do (
  where %%T >nul 2>&1
  if errorlevel 1 (
    set "ERR=%%T was not found on PATH ^(see the header of this script^)"
    goto :fail
  )
)
echo   git cmake tar curl certutil ... ok

set "ERR=no usable g++.exe - the vendored toolchain is missing from third_party\toolchain, and no g++ was found on PATH. See DEPENDENCIES.md."
call "%ROOT%\find_gcc.bat"
if errorlevel 1 goto :fail

rem cmake drives ninja, and both ninja and the compiler live in the toolchain
rem bin folder, which is not on PATH by default.
set "PATH=%MINI_RTC_GCC%;%PATH%"
where ninja >nul 2>&1
if errorlevel 1 (
  set "ERR=ninja was not found next to g++ - the toolchain in third_party\toolchain looks incomplete or truncated"
  goto :fail
)

if not exist "%TP%" mkdir "%TP%"
if not exist "%CACHE%" mkdir "%CACHE%"

rem ===========================================================================
rem 1. OpenSSL - headers and import libraries.
rem    Runs first because libdatachannel's configure needs the OpenSSL headers.
rem    Only link-time artifacts are installed; dist\x64 keeps the pinned 3.6.3
rem    runtime DLLs (see DEPENDENCIES.md).
rem ===========================================================================
echo.
call :step "1/3  OpenSSL %OSSL_VERSION% - headers + import libs (ucrt64)"
set "OSSL_PKG=%CACHE%\openssl-ucrt64.pkg.tar.zst"
if exist "%OPENSSL_PREFIX%\lib\libcrypto.dll.a" if not defined FORCE (
  echo   already installed - skipping ^(set MINI_RTC_FORCE=1 to redo^)
  goto :openssl_done
)
call :fetch "%OSSL_URL%" "%OSSL_PKG%" "%OSSL_SHA%" "OpenSSL ucrt64 package"
if errorlevel 1 goto :fail
if exist "%CACHE%\ossl-pkg" rmdir /s /q "%CACHE%\ossl-pkg"
mkdir "%CACHE%\ossl-pkg" >nul 2>&1
call :step "       extracting package"
tar -xf "%OSSL_PKG%" -C "%CACHE%\ossl-pkg"
if errorlevel 1 (
  set "ERR=could not read the .tar.zst package - Windows 10 1803+ tar handles zstd, otherwise extract with 7-Zip ZS or NanaZip"
  goto :fail
)
if not exist "%CACHE%\ossl-pkg\ucrt64\include\openssl\ssl.h" (
  set "ERR=unexpected package layout - ucrt64\include\openssl\ssl.h is missing, so the MSYS2 package layout has changed"
  goto :fail
)
call :step "       installing into third_party\openssl-mingw64"
if not exist "%OPENSSL_PREFIX%\lib" mkdir "%OPENSSL_PREFIX%\lib" >nul 2>&1
xcopy /y /i /q "%CACHE%\ossl-pkg\ucrt64\include\openssl" "%OPENSSL_PREFIX%\include\openssl" >nul
if errorlevel 1 (
  set "ERR=failed to install the OpenSSL headers"
  goto :fail
)
copy /y "%CACHE%\ossl-pkg\ucrt64\lib\libcrypto.dll.a" "%OPENSSL_PREFIX%\lib\" >nul
if errorlevel 1 (
  set "ERR=failed to install libcrypto.dll.a"
  goto :fail
)
copy /y "%CACHE%\ossl-pkg\ucrt64\lib\libssl.dll.a" "%OPENSSL_PREFIX%\lib\" >nul
if errorlevel 1 (
  set "ERR=failed to install libssl.dll.a"
  goto :fail
)
:openssl_done
if not exist "%OPENSSL_PREFIX%\lib\libcrypto.dll.a" (
  set "ERR=libcrypto.dll.a is missing from %OPENSSL_PREFIX%\lib"
  goto :fail
)
echo   ok

rem ===========================================================================
rem 2. libopus - static codec library.
rem ===========================================================================
echo.
call :step "2/3  libopus %OPUS_VERSION% - static"
set "OPUS_TGZ=%CACHE%\opus-%OPUS_VERSION%.tar.gz"
set "OPUS_SRC=%TP%\opus-%OPUS_VERSION%-src"
if exist "%OPUS_PREFIX%\lib\libopus.a" if not defined FORCE (
  echo   already installed - skipping ^(set MINI_RTC_FORCE=1 to redo^)
  goto :opus_done
)
call :fetch "%OPUS_URL%" "%OPUS_TGZ%" "%OPUS_SHA%" "libopus tarball"
if errorlevel 1 goto :fail
if exist "%OPUS_SRC%" rmdir /s /q "%OPUS_SRC%"
mkdir "%OPUS_SRC%" >nul 2>&1
call :step "       extracting and building"
rem --strip-components=1 drops the opus-<ver>\ wrapper directory
tar -xzf "%OPUS_TGZ%" -C "%OPUS_SRC%" --strip-components=1
if errorlevel 1 (
  set "ERR=failed to extract %OPUS_TGZ%"
  goto :fail
)
set "LOG=%CACHE%\opus.log"
rem cmake wants forward slashes in -D values
set "OPUSA=%OPUS_PREFIX:/=/%"
cmake -S "%OPUS_SRC%" -B "%OPUS_SRC%\build" -G Ninja ^
  -DCMAKE_C_COMPILER="%MINI_RTC_GCC%\gcc.exe" ^
  -DCMAKE_CXX_COMPILER="%MINI_RTC_GCC%\g++.exe" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%OPUSA%" ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DOPUS_BUILD_PROGRAMS=OFF ^
  -DOPUS_BUILD_TESTING=OFF >"%LOG%" 2>&1
if errorlevel 1 (
  set "ERR=libopus cmake configure failed"
  goto :fail
)
cmake --build "%OPUS_SRC%\build" >>"%LOG%" 2>&1
if errorlevel 1 (
  set "ERR=libopus build failed"
  goto :fail
)
cmake --install "%OPUS_SRC%\build" >>"%LOG%" 2>&1
if errorlevel 1 (
  set "ERR=libopus install failed"
  goto :fail
)
:opus_done
if not exist "%OPUS_PREFIX%\lib\libopus.a" (
  set "ERR=libopus.a is missing from %OPUS_PREFIX%\lib"
  goto :fail
)
echo   ok

rem ===========================================================================
rem 3. libdatachannel + libjuice + usrsctp + libsrtp.
rem    These four static libraries are what build.bat links.
rem
rem    Two non-obvious requirements are encoded below:
rem      - BUILD_SHARED_LIBS defaults to ON, which would produce a shared
rem        libdatachannel.dll instead of the libdatachannel.a we link.
rem      - libsrtp and usrsctp enable warnings-as-errors by default, and GCC
rem        16 promotes their warnings (e.g. -Wunused-but-set-variable) to hard
rem        build failures. Both must be switched off explicitly.
rem    The explicit OPENSSL_* cache entries are also required: CMake 4.3 does
rem    not honour OPENSSL_ROOT_DIR on its own for these projects.
rem ===========================================================================
echo.
call :step "3/3  libdatachannel %DC_VERSION% + deps - static"
if exist "%DC%\include\rtc\rtc.hpp" (
  if defined FORCE (
    rmdir /s /q "%DC%"
  ) else (
    echo   source already cloned - refreshing submodules
    pushd "%DC%" >nul 2>&1
    git submodule update --init --recursive --depth 1 >nul 2>&1
    popd >nul 2>&1
  )
)
if not exist "%DC%\include\rtc\rtc.hpp" (
  call :step "       cloning (submodules are required - the source zip has empty deps/)"
  rmdir /s /q "%DC%" >nul 2>&1
  rem GitHub answers unauthenticated rate limiting with a 404-style
  rem "Repository not found", so a plain retry can hit the same limit
  rem immediately. Back off between attempts.
  set "CLONED="
  for /l %%A in (1,1,4) do (
    if not defined CLONED (
      if %%A gtr 1 (
        echo   retry %%A-1 of 3 after a rate limit ^(waiting 10s^)
        ping -n 11 127.0.0.1 >nul 2>&1
      )
      rem stdout to nul (the progress meter is hundreds of lines); stderr is
      rem kept so a real failure - e.g. GitHub's 404-style rate limiting -
      rem is still visible.
      git clone --depth 1 -b %DC_TAG% --recurse-submodules --shallow-submodules "%DC_REPO%" "%DC%" >nul
      if not errorlevel 1 set "CLONED=1"
    )
  )
  if not defined CLONED (
    set "ERR=git clone of libdatachannel failed after 4 attempts - check network access to github.com, and note that unauthenticated GitHub git access is rate limited"
    goto :fail
  )
)

call :step "       configuring"
rem A cache generated at a different path makes cmake refuse to run, so the
rem build tree is always regenerated.
if exist "%DC%\build" rmdir /s /q "%DC%\build"
set "OSSLA=%OPENSSL_PREFIX:/=/%"
set "LOG=%CACHE%\libdatachannel-configure.log"
cmake -S "%DC%" -B "%DC%\build" -G Ninja ^
  -DCMAKE_C_COMPILER="%MINI_RTC_GCC%\gcc.exe" ^
  -DCMAKE_CXX_COMPILER="%MINI_RTC_GCC%\g++.exe" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DNO_EXAMPLES=ON ^
  -DNO_TESTS=ON ^
  -DUSE_NICE=OFF ^
  -DOPENSSL_USE_STATIC_LIBS=FALSE ^
  -DOPENSSL_ROOT_DIR="%OSSLA%" ^
  -DOPENSSL_INCLUDE_DIR="%OSSLA%/include" ^
  -DOPENSSL_CRYPTO_LIBRARY="%OSSLA%/lib/libcrypto.dll.a" ^
  -DOPENSSL_SSL_LIBRARY="%OSSLA%/lib/libssl.dll.a" ^
  -DENABLE_WARNINGS_AS_ERRORS=OFF ^
  -Dsctp_werror=OFF ^
  -DWARNINGS_AS_ERRORS=OFF >"%LOG%" 2>&1
if errorlevel 1 (
  set "ERR=libdatachannel cmake configure failed"
  goto :fail
)
call :step "       building (a few minutes)"
set "LOG=%CACHE%\libdatachannel-build.log"
cmake --build "%DC%\build" >"%LOG%" 2>&1
if errorlevel 1 (
  set "ERR=libdatachannel build failed"
  goto :fail
)

set "MISSING="
for %%F in (
  "%DC%\build\libdatachannel.a"
  "%DC%\build\deps\libsrtp\libsrtp2.a"
  "%DC%\build\deps\libjuice\libjuice.a"
  "%DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a"
) do (
  if not exist %%F set "MISSING=!MISSING! %%~nxF"
)
if defined MISSING (
  set "ERR=the build did not produce these expected static libraries:!MISSING!"
  goto :fail
)
echo   ok

rem ===========================================================================
echo.
echo === all dependencies ready ===
echo.
echo   OpenSSL        %OPENSSL_PREFIX%
echo   libopus        %OPUS_PREFIX%
echo   libdatachannel %DC%
echo.
echo Next:  build.bat
echo.
endlocal
exit /b 0

rem ---- failure path ---------------------------------------------------------
rem Reached via "goto :fail". Prints ERR plus LOG when one is set, then stops
rem the whole script. (An "exit /b" inside a called subroutine would only
rem leave that subroutine, which is why this is a goto and not a call.)
:fail
echo.
echo ERROR: %ERR%
if defined LOG (
  echo        full log: %LOG%
) else (
  echo        See DEPENDENCIES.md for background and troubleshooting.
)
endlocal
exit /b 1

rem ---- helpers (no setlocal/endlocal here - they would clobber the caller) ---

:step
echo %~1
exit /b 0

rem :fetch <url> <destfile> <sha256> <label>
:fetch
if exist "%~2" if not defined FORCE (
  echo   cached %~4
  goto :verify
)
echo   downloading %~4
curl -fsSL --retry 3 --retry-delay 2 -A "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" -o "%~2" "%~1"
if errorlevel 1 (
  set "ERR=download failed: %~1"
  exit /b 1
)
:verify
certutil -hashfile "%~2" SHA256 | findstr /i /c:"%~3" >nul
if errorlevel 1 (
  set "ERR=checksum mismatch for %~2 - corrupt download, or the pinned version moved. Expected SHA256 %~3"
  exit /b 1
)
echo   checksum ok
exit /b 0
