# Dependencies

`webrtc_api.dll` is `__cdecl` / x64 / UCRT (Windows 10+). It links
libdatachannel, libjuice, usrsctp, libsrtp and libopus **statically**, and
OpenSSL **dynamically** (`libssl-3-x64.dll` + `libcrypto-3-x64.dll` must sit
next to it at run time — those two are the *only* runtime dependencies;
`libwinpthread-1.dll` is **not** imported).

## Toolchain

| | |
|---|---|
| Compiler | GCC 16.1.0 / MinGW-w64 14.0.0, **x86_64-ucrt-posix-seh** (winlibs r3) |
| Default path | `C:\Users\archl\Documents\winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64ucrt-14.0.0-r3\mingw64\bin` |

The compiler is a *toolchain*, not a project dependency, so it deliberately
stays outside the repository. Override with `set MINI_RTC_GCC=<path>` before
running `build.bat`.

## Versions

Dependencies live in `third_party\` next to `build.bat`, which is the default
for every path below. `third_party\` is not tracked by git; see
`.gitignore`.

| Component | Version | Install prefix (`MINI_RTC_*`) |
|---|---|---|
| libdatachannel | 0.24.5 (`443f6934d9007eb7076ab7825ba330f355fcbead`) | `MINI_RTC_DC` |
| libjuice | `3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6` | built under `MINI_RTC_DC\build` |
| libsrtp | `24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5` | built under `MINI_RTC_DC\build` |
| usrsctp | `fec583d54493f879d2ae44a743423bf8a04371ab` | built under `MINI_RTC_DC\build` |
| plog | `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` | built under `MINI_RTC_DC\build` |
| nlohmann/json | `55f93686c01528224f448c19128836e7df245f72` | header-only |
| libopus | 1.5.2 | `MINI_RTC_OPUS` |
| OpenSSL (headers + import libs) | 3.6.4 | `MINI_RTC_OPENSSL` |
| OpenSSL (shipped runtime DLLs) | 3.6.3 | `dist\x64\` |

### Why OpenSSL is 3.6.4 headers but a 3.6.3 runtime

The 3.6.3 source tarball cannot be configured on this machine: it has no
`CMakeLists.txt` and needs `perl` to run `Configure` (28 headers such as
`ssl.h`, `crypto.h` and `configuration.h` are generated), and **no perl is
installed**. The link-time artifacts therefore come from the MSYS2 `ucrt64`
package — the same UCRT toolchain as the compiler — while the *shipped* runtime
stays the 3.6.3 DLLs already committed in `dist\x64`.

This is safe: OpenSSL keeps ABI compatibility across a 3.6.x series, and all
**113** OpenSSL symbols the wrapper imports (77 `libcrypto`, 36 `libssl`) were
verified to be exported by the shipped 3.6.3 DLLs. Re-run
`tests/verify_build.ps1`-style checking, or simply load the DLL, after any
OpenSSL bump.

## Obtaining them

The quickest route is to let the scripts do it:

```bat
build_libraries.bat     rem download, verify and build every dependency
build.bat               rem compile webrtc_api.dll into dist\x64
```

`build_libraries.bat` installs into `third_party\`, verifying every download
against the pinned SHA256 and writing cmake output to
`third_party\_downloads\*.log` on failure. Re-running it is cheap: anything
already present is skipped unless you set `MINI_RTC_FORCE=1`.

The toolchain is **vendored** at `third_party\toolchain\` so a build does not
depend on what happens to be installed on the machine. `find_gcc.bat` resolves
the compiler in this order:

1. `MINI_RTC_GCC`, if it already points at a usable `g++.exe`
2. `third_party\toolchain\mingw64\bin`
3. `g++` on `PATH`

No common install directories are searched, on purpose. It verifies the
compiler reports `x86_64-w64-mingw32` and warns if no `libucrt.a` is present
(that would mean an msvcrt build, which will not link).

### Doing it by hand

```sh
# 1. libdatachannel 0.24.5 with pinned submodules
git clone --depth 1 -b v0.24.5 --recurse-submodules --shallow-submodules \
  https://github.com/paullouisageneau/libdatachannel.git \
  third_party/libdatachannel-0.24.5

# 2. libdatachannel static libs (needed before build.bat will run)
cmake -S <MINI_RTC_DC> -B <MINI_RTC_DC>\build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ^
  -DNO_EXAMPLES=ON -DNO_TESTS=ON -DUSE_NICE=OFF ^
  -DOPENSSL_USE_STATIC_LIBS=FALSE ^
  -DOPENSSL_ROOT_DIR=<MINI_RTC_OPENSSL> ^
  -DOPENSSL_INCLUDE_DIR=<MINI_RTC_OPENSSL>\include ^
  -DOPENSSL_CRYPTO_LIBRARY=<MINI_RTC_OPENSSL>\lib\libcrypto.dll.a ^
  -DOPENSSL_SSL_LIBRARY=<MINI_RTC_OPENSSL>\lib\libssl.dll.a ^
  -DENABLE_WARNINGS_AS_ERRORS=OFF -Dsctp_werror=OFF -DWARNINGS_AS_ERRORS=OFF
cmake --build <MINI_RTC_DC>\build

# 3. libopus 1.5.2
#    sha256 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
curl -LO https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
cmake -S opus-1.5.2 -B opus-1.5.2/build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX=<MINI_RTC_OPUS> -DBUILD_SHARED_LIBS=OFF ^
  -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmake --build opus-1.5.2/build && cmake --install opus-1.5.2/build

# 4. OpenSSL headers + import libs (ucrt64)
#    sha256 0b7b6497910450d9c9c0feef66f00493dc54f4afeab1e1144447e8f9d6e4366f
curl -LO https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-openssl-3.6.4-1-any.pkg.tar.zst
# tar -xf understands the zstd container; then copy:
#   ucrt64/include/openssl        -> <MINI_RTC_OPENSSL>\include\openssl
#   ucrt64/lib/lib{ssl,crypto}.dll.a -> <MINI_RTC_OPENSSL>\lib
```

### Gotchas that will bite again

- `-DBUILD_SHARED_LIBS=OFF` is **required**. libdatachannel defaults it to
  `ON`, which produces a shared `libdatachannel.dll` instead of the
  `libdatachannel.a` that `build.bat` links.
- 0.24.5 dropped the old `-static` file names. The libraries are
  `libdatachannel.a` and `libjuice.a` — *not* `libdatachannel-static.a` /
  `libjuice-static.a`. Older notes referring to the `-static` names are wrong.
- `-DENABLE_WARNINGS_AS_ERRORS=OFF -Dsctp_werror=OFF` is **required** with
  GCC 16. libsrtp and usrsctp default to `-Werror` and emit warnings that newer
  GCC turns into build failures (e.g. `-Wunused-but-set-variable` in
  `sctp_usrreq.c`).
- `CMakeLists.txt` must be given the OpenSSL paths as explicit
  `OPENSSL_INCLUDE_DIR` / `OPENSSL_CRYPTO_LIBRARY` / `OPENSSL_SSL_LIBRARY`
  cache entries; `OPENSSL_ROOT_DIR` alone is not honoured by CMake 4.3.
- The GitHub source zip of libdatachannel ships **empty** `deps/*` submodule
  directories — clone with `--recurse-submodules` instead of unzipping.
- GitHub answers unauthenticated rate limiting with a **404-style
  "Repository not found"**, not a 429. A clone that fails that way may just need
  a retry, so `build_libraries.bat` backs off and retries up to 4 times.
- Batch files must be checked out with **CRLF**. `cmd.exe` cannot reliably
  resolve `call :label` / `goto :label` in LF-only files — it fails at runtime
  with "The system cannot find the batch label specified", often only on some
  code paths, which makes it genuinely hard to debug. `.gitattributes` pins
  `*.bat text eol=crlf` to stop this regressing.

## Verifying a build

`build.bat` writes `dist\x64\webrtc_api.dll` and copies it to the top level.
After building, confirm the export table is unchanged and that the DLL loads:

```sh
objdump -p dist/x64/webrtc_api.dll | findstr net_    # expect 31 net_* exports
```

A DLL that imports a symbol absent from the shipped OpenSSL runtime will load
with a `STATUS_ENTRYPOINT_NOT_FOUND` failure, so always load-test (the
`python_demo` scripts do this implicitly) after changing the OpenSSL version.

### What "reproducible" means here

`-Wl,--no-insert-timestamp` makes repeated builds **at the same path** produce
byte-identical output, verified by building twice and comparing SHA256. Builds
from *different* directories are not bit-identical: libjuice embeds its source
paths through `__FILE__`, so the checkout path ends up in the binary. That is
inherent to the vendored dependency, and the DLL is functionally equivalent
either way.
