# Third-party licenses

This project's own code (`webrtc_api.cpp`, demo scripts, docs) is MIT licensed
(see `LICENSE`). However, the distributed DLLs are built on third-party
libraries under their own licenses:

## Libraries statically linked into `webrtc_api.dll`

| Library | License | Upstream |
|---|---|---|
| libdatachannel | MPL-2.0 | https://github.com/paullouisageneau/libdatachannel |
| libjuice | MPL-2.0 | https://github.com/paullouisageneau/libjuice |
| usrsctp | BSD-3-Clause | https://github.com/sctplab/usrsctp |

`webrtc_api.cpp` includes the header `<rtc/rtc.h>` (MPL-2.0) at build time.

Per the MPL-2.0, the source for these libraries is available at the upstream
repositories linked above. Compiled object code is distributed in
`dist/x64/webrtc_api.dll`.

## Dynamically linked DLLs shipped alongside

| File | License | Upstream |
|---|---|---|
| libssl-3-x64.dll | Apache-2.0 | https://github.com/openssl/openssl |
| libcrypto-3-x64.dll | Apache-2.0 | https://github.com/openssl/openssl |

OpenSSL 3 is licensed under Apache-2.0; the full license text is available at
https://github.com/openssl/openssl/blob/master/LICENSE.txt
