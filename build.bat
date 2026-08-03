@echo off
rem Build webrtc_api.dll (GameMaker WebRTC wrapper) from libdatachannel source.
rem Requirements (all local):
rem   - winlibs g++ (ucrt) toolchain
rem   - C:\Users\archl\Documents\libdatachannel  (cloned with submodules, built)
rem   - C:\Users\archl\Documents\openssl-mingw64  (MSYS2 mingw64 OpenSSL package)
set GCC=C:\Users\archl\Documents\winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64ucrt-14.0.0-r3\mingw64\bin
set DC=C:\Users\archl\Documents\libdatachannel
set OS=C:\Users\archl\Documents\openssl-mingw64\mingw64
set INC=%DC%\include;%DC%\build

echo Compiling wrapper DLL (static CRT, dynamic OpenSSL) ...
"%GCC%\g++.exe" -shared -O2 -static -o webrtc_api.dll webrtc_api.cpp -I"%DC%\include" -I"%DC%\build" ^
  "%DC%\build\libdatachannel-static.a" ^
  "%DC%\build\deps\libjuice\libjuice-static.a" ^
  "%DC%\build\deps\usrsctp\usrsctplib\libusrsctp.a" ^
  "%OS%\lib\libssl.dll.a" "%OS%\lib\libcrypto.dll.a" ^
  -lws2_32 -lole32 -liphlpapi -lbcrypt -lcrypt32
if errorlevel 1 goto :fail
echo BUILD OK - deploy webrtc_api.dll + libssl-3-x64.dll + libcrypto-3-x64.dll
goto :eof
:fail
echo BUILD FAILED
