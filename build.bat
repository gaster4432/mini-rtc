@echo off
rem Build webrtc_api.dll (GameMaker WebRTC wrapper).
rem Hard-coded paths for this machine:
rem   GCC     = winlibs MinGW x64 bin folder
rem   DC      = libdatachannel root
rem   OPENSSL = mingw64 OpenSSL root
rem   OP      = mingw64 opus root
setlocal

set "GCC=C:\Users\archl\Documents\winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64ucrt-14.0.0-r3\mingw64\bin"
set "DC=C:\Users\archl\Documents\libdatachannel"
set "OPENSSL=C:\Users\archl\Documents\openssl-mingw64\mingw64"
set "OP=C:\Users\archl\Documents\opus-mingw64"

if not exist "%GCC%\g++.exe" (
  echo ERROR: g++.exe not found at %GCC%
  exit /b 1
)

echo Using GCC=%GCC%
echo Using DC=%DC%
echo Using OPENSSL=%OPENSSL%
echo Using OP=%OP%

echo Compiling wrapper DLL (static CRT, dynamic OpenSSL) ...
"%GCC%\g++.exe" -shared -O2 -static -o webrtc_api.dll webrtc_api.cpp -I"%DC%\include" -I"%DC%\build" -I"%OP%\include" ^
  "%DC%\build\libdatachannel-static.a" ^
  "%DC%\build\deps\libsrtp\libsrtp2.a" ^
  "%DC%\build\deps\libjuice\libjuice-static.a" ^
  "%DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a" ^
  "%OP%\lib\libopus.a" ^
  "%OPENSSL%\lib\libssl.dll.a" "%OPENSSL%\lib\libcrypto.dll.a" ^
  -lws2_32 -lole32 -liphlpapi -lbcrypt -lcrypt32 -luuid

if errorlevel 1 (
  echo BUILD FAILED
  endlocal
  exit /b 1
)
echo BUILD OK - deploy webrtc_api.dll + libssl-3-x64.dll + libcrypto-3-x64.dll
endlocal
exit /b 0