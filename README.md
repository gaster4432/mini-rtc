# mini-rtc — a bare-bones WebRTC wrapper (x64 only)

> **Architecture: x64 (64-bit Windows only).** These DLLs will not load in
> 32-bit or ARM apps. Use an x64 build of your app.

A single-header-style WebRTC DLL for anyone who wants peer-to-peer data channels
without touching WebRTC internals. One DLL + two OpenSSL DLLs, load it, call a
handful of `net_*` functions, poll events, done.

Built on [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
(static) + OpenSSL 3 (dynamic), compiled with MinGW g++ for x64. No servers are
hardcoded anywhere — signaling is 100% up to you (HTTP, WebSocket, paste into
Discord, whatever).

---

## 1. Files

```
webrtc_api.dll        <- the wrapper (x64, __cdecl exports)
libssl-3-x64.dll      <- required runtime dependency
libcrypto-3-x64.dll   <- required runtime dependency
libwinpthread-1.dll   <- required on some mingw builds
```

Keep all four in the same folder as your app. Source and the build script are
included for reproducibility (`webrtc_api.cpp`, `build.bat`).

---

## 2. Getting connected — the big picture

```
   Peer A (host)                       relay (your choice)        Peer B (guest)
        |  net_create_pc()                                     net_create_pc()
        |  net_create_dc("chat")                                      |
        |  net_create_offer()                                        |
        |  event 1: local SDP  ----------------->  mailbox code "1234" <--- poll
        |  event 2: local ICE  ----------------->                     <--- poll
        |                                                                net_set_remote(sdp, "offer")
        |                                                            net_create_answer()
        |  <----------------------------- event 1: local answer SDP
        |  net_set_remote(answer, "answer")
        |  <------------------------------ ICE candidates both ways: net_add_ice(mid, 0, cand)
        |  event 3: data channel open                                event 3: data channel open
        |  net_send("hi")  ======================================>   event 4: "hi"
```

Only three things matter for the handshake:
1. **A room code** both sides agree on (any unique string — it's just the mailbox key).
2. **Sending SDP** (events type 1) and **ICE** (event type 2) to the other side.
3. **Feeding received SDP** (`net_set_remote`) and **ICE** (`net_add_ice`) back in.

The relay is optional. You can exchange those strings by hand over chat, or run
a ~20-line HTTP mailbox (see `python_demo/relay.py`).

---

## 3. API reference

All functions are `__cdecl` and exported with C linkage — bindable from
GameMaker (`external_define(..., dll_cdecl, ...)`), C/C++ (`LoadLibrary` +
`GetProcAddress`), Python (`ctypes.CDLL`), Rust, etc. Numbers are doubles,
strings are UTF-8 `char*`.

### Lifecycle

| Function | Signature | Notes |
|---|---|---|
| `net_version` | `double net_version()` | returns 2.0 |
| `net_init` | `double net_init()` | call once at startup |
| `net_terminate` | `void net_terminate()` | close everything, cleanup |
| `net_close` | `void net_close()` | close pc + data channel, keep dll loaded |
| `net_add_ice_server` | `double net_add_ice_server(const char* uri)` | e.g. `"stun:stun.l.google.com:19302"`; if none added, Google STUN is used automatically |
| `net_state` | `double net_state()` | last rtcState (`-1` if no pc) |

### Peer connection

| Function | Signature | Notes |
|---|---|---|
| `net_create_pc` | `double net_create_pc()` | 1 on success, 0 on failure |
| `net_create_offer` | `double net_create_offer()` | start handshake as host |
| `net_create_answer` | `double net_create_answer()` | after receiving remote offer |
| `net_set_remote` | `double net_set_remote(const char* sdp, const char* type)` | type: `"offer"`/`"answer"` (or empty = auto) |
| `net_set_local` | `double net_set_local(const char* sdp, const char* type)` | convenience; type is the negotiation type, sdp ignored (local desc is generated) |
| `net_add_ice` | `double net_add_ice(const char* mid, double index, const char* cand)` | index unused; mid may be empty string |
| `net_create_dc` | `double net_create_dc(const char* label)` | returns data channel id (>= 0) |

### Sending

| Function | Signature | Notes |
|---|---|---|
| `net_send` | `double net_send(const char* str)` | text on the last data channel |
| `net_send_to` | `double net_send_to(double id, const char* str)` | text on a specific channel |
| `net_send_buf` | `double net_send_buf(int64 addr, double size)` | binary on last channel |
| `net_send_buf_to` | `double net_send_buf_to(double id, int64 addr, double size)` | binary on a specific channel |
| `net_voice` | `double net_voice(double on)` | stub — returns 0 (no audio in this build) |

### Receiving (event polling)

Pull events in a loop (one per step in a game, or every 10-50 ms elsewhere).
`net_poll` returns the next event type, or **-1** if none. After a successful
poll, the event payload is read with the `net_event_*` getters **before**
calling `net_poll` again.

| Function | Signature | Returns |
|---|---|---|
| `net_poll` | `double net_poll()` | event type, or -1 if none |
| `net_pop` | `void net_pop()` | drop the current event (not usually needed) |
| `net_event_id` | `double net_event_id()` | pc/dc id for the event |
| `net_event_int` | `double net_event_int()` | `n` field (size / state) |
| `net_event_string` | `const char* net_event_string()` | `s1` field (text / sdp / cand / error) |
| `net_event_string2` | `const char* net_event_string2()` | `s2` field (sdp type / mid) |
| `net_event_data` | `double net_event_data(int64 addr, double maxsize)` | copies binary payload to your buffer, returns bytes copied |

### Event types

| Type | Meaning | Payload |
|---|---|---|
| 1 | local SDP ready | s1 = sdp, s2 = `"offer"` / `"answer"` — **send this to the peer** |
| 2 | local ICE candidate | s1 = candidate, s2 = mid — **send this to the peer** |
| 3 | data channel open | id = dc id — ready to `net_send` |
| 4 | text message received | s1 = text, n = length |
| 6 | remote data channel created | id = dc id |
| 7 | data channel closed | id = dc id |
| 8 | pc connection state changed | n = rtcState |
| 9 | error | s1 = error message |
| 10 | binary message received | n = size, bytes via `net_event_data` |

rtcState values (libdatachannel): `0` new, `1` connecting, `2` connected,
`3` disconnected, `4` failed, `5` closed.

Text vs binary: incoming messages are auto-classified — printable bytes (no NUL,
no control chars) arrive as type 4, everything else as type 10.

---

## 4. Minimum handshake (pseudocode, both sides)

```c
net_init();
net_add_ice_server("stun:stun.l.google.com:19302");
net_create_pc();

/* host only */
net_create_dc("chat");
net_create_offer();

loop {
  switch (net_poll()) {
  case 1: /* sdp */   send_to_peer(net_event_string(), net_event_string2()); break;  // sdp, type
  case 2: /* ice */   send_to_peer(net_event_string(), net_event_string2()); break;  // cand, mid
  case 3: /* open */  net_send("hello!"); break;
  case 4: /* text */  print(net_event_string()); break;
  case -1: /* idle */ break;
  }
  /* incoming from peer */
  while (msg = recv_from_peer()) {
    if (msg is sdp) net_set_remote(msg.sdp, msg.type);
    if (msg is ice) net_add_ice(msg.mid, 0, msg.cand);
  }
}
```

---

## 5. Signaling: the worker API or your own relay

A tiny Cloudflare worker implements a mailbox (`worker/worker.js`, a Durable
Object per room code):

```
POST /create           -> { ok, code }
POST /join              { code }   -> { ok, code }
POST /signal?code=X     { from, target, msg }   (target "" = everyone)
GET  /poll?code=X&role=R           -> { ok, msgs: [...] }
```

`create`/`join` are optional convenience — the protocol only needs
`signal` + `poll` with any agreed-upon code. A self-hosted replacement is
~20 lines (see `python_demo/relay.py`). The DLL never talks to the relay;
your glue code does, and feeds results to `net_set_remote`/`net_add_ice`.

---

## 6. Rebuilding (x64)

Requirements: winlibs MinGW x64 g++ and a libdatachannel build tree + OpenSSL
for MinGW. See `build.bat` for the exact paths, include dirs and link line
(libdatachannel-static.a + libjuice-static.a + libusrsctp.a + OpenSSL DLL
imports + ws2_32/ole32/iphlpapi/bcrypt/crypt32).

---

## 7. Python demo

See `python_demo/`:
- `relay.py` — self-hosted signaling mailbox (no Cloudflare needed)
- `demo.py` — two-terminal chat demo using the DLL via ctypes

Run:
```
terminal 1:  python demo.py host [--relay http://localhost:8000]
terminal 2:  python demo.py guest <code> [--relay http://localhost:8000]
```
(Default relay is the public Cloudflare worker; add `--relay` to use your own.)

---

## Acknowledgments

Special thanks to opencode (an AI coding assistant) for development help.
