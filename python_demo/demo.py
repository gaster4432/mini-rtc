"""demo.py - bare-bones P2P chat using webrtc_api.dll via ctypes.

   Usage:
     terminal 1:  python demo.py host
     terminal 2:  python demo.py guest <code>

   Optional:  --relay http://localhost:8000   (use relay.py instead of the
              public Cloudflare worker)

   Type a message + Enter to send. Ctrl+C to quit.
"""
import ctypes, json, random, string, sys, threading, time, urllib.request

DLL_PATH = "../dist/x64/webrtc_api.dll"   # change per arch / your setup
RELAY = "https://ai-game-relay.archlinuxkid99.workers.dev"

# ---------------- DLL bindings (__cdecl) ----------------
dll = ctypes.CDLL(DLL_PATH)
dll.net_version.restype = ctypes.c_double
dll.net_init.restype = ctypes.c_double
dll.net_terminate.restype = None
dll.net_close.restype = None
dll.net_add_ice_server.restype = ctypes.c_double
dll.net_add_ice_server.argtypes = [ctypes.c_char_p]
dll.net_create_pc.restype = ctypes.c_double
dll.net_create_offer.restype = ctypes.c_double
dll.net_create_answer.restype = ctypes.c_double
dll.net_set_remote.restype = ctypes.c_double
dll.net_set_remote.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
dll.net_add_ice.restype = ctypes.c_double
dll.net_add_ice.argtypes = [ctypes.c_char_p, ctypes.c_double, ctypes.c_char_p]
dll.net_create_dc.restype = ctypes.c_double
dll.net_create_dc.argtypes = [ctypes.c_char_p]
dll.net_send.restype = ctypes.c_double
dll.net_send.argtypes = [ctypes.c_char_p]
dll.net_poll.restype = ctypes.c_double
dll.net_event_string.restype = ctypes.c_char_p
dll.net_event_string2.restype = ctypes.c_char_p
dll.net_event_int.restype = ctypes.c_double

# ---------------- relay helpers ----------------
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) webrtc-demo/1.0"}
def http_get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=10) as r:
        return json.loads(r.read().decode())

def http_post(url, obj):
    req = urllib.request.Request(url, json.dumps(obj).encode(), {"Content-Type": "application/json", **UA})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())

def poll(relay, code, role):
    try:
        return http_get(f"{relay}/poll?code={code}&role={role}")["msgs"]
    except Exception:
        return []

def signal(relay, code, role, target, msg):
    try:
        http_post(f"{relay}/signal?code={code}", {"from": role, "target": target, "msg": msg})
    except Exception as e:
        print(f"[relay error] {e}")

# ---------------- main ----------------
def main():
    role = sys.argv[1] if len(sys.argv) > 1 else "host"
    relay = RELAY
    if "--relay" in sys.argv:
        relay = sys.argv[sys.argv.index("--relay") + 1]
    if role == "guest" and len(sys.argv) > 2 and not sys.argv[2].startswith("--"):
        code = sys.argv[2]
    else:
        code = "".join(random.choices(string.digits, k=4))
        print(f"room code: {code}")

    target = "guest" if role == "host" else "host"
    peer_state = {"has_sdp": False, "dc_open": False}

    dll.net_init()
    dll.net_add_ice_server(b"stun:stun.l.google.com:19302")
    print(f"loaded webrtc_api.dll v{dll.net_version():.1f}  relay={relay}  code={code}")

    if role == "host":
        dll.net_create_pc()
        dll.net_create_dc(b"chat")
        dll.net_create_offer()
        print("hosting... waiting for guest")
    else:
        dll.net_create_pc()
        print("joining... waiting for host")

    # background: pull relay messages -> feed DLL
    def inbox():
        while True:
            for m in poll(relay, code, role):
                try:
                    p = json.loads(m)
                except Exception:
                    continue
                if p.get("type") == "sdp" and not peer_state["has_sdp"]:
                    dll.net_set_remote(p["sdp"].encode(), p.get("sdpType", "offer").encode())
                    peer_state["has_sdp"] = True
                    if role == "guest":
                        dll.net_create_answer()
                elif p.get("type") == "ice":
                    dll.net_add_ice(p.get("mid", "").encode(), 0, p["cand"].encode())
            time.sleep(0.05)
    threading.Thread(target=inbox, daemon=True).start()

    def outbox():
        while True:
            t = dll.net_poll()
            if t == -1:
                time.sleep(0.01); continue
            if t == 1:  # local SDP
                signal(relay, code, role, target,
                       {"type": "sdp", "sdp": dll.net_event_string().decode(),
                        "sdpType": dll.net_event_string2().decode()})
                print("[signaling] sdp sent")
            elif t == 2:  # local ICE
                signal(relay, code, role, target,
                       {"type": "ice", "mid": dll.net_event_string2().decode(),
                        "cand": dll.net_event_string().decode()})
            elif t == 3:  # dc open
                peer_state["dc_open"] = True
                print("\n=== CONNECTED! type to chat (Ctrl+C to quit) ===")
            elif t == 4:  # text
                print(f"\r<peer> {dll.net_event_string().decode()}\n> ", end="")
            elif t == 8:  # state change
                st = dll.net_event_int()
                if st in (3, 4, 5):
                    print(f"\n[connection closed/failed state={st:.0f}]")
            elif t == 9:  # error
                print(f"\n[error] {dll.net_event_string().decode()}")
    threading.Thread(target=outbox, daemon=True).start()

    try:
        while True:
            msg = input("> " if peer_state["dc_open"] else "[waiting] ")
            if msg and peer_state["dc_open"]:
                dll.net_send(msg.encode())
    except (KeyboardInterrupt, EOFError):
        pass
    dll.net_terminate()
    print("bye")

if __name__ == "__main__":
    main()
