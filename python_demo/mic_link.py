"""mic_link.py - one-machine mic relay over localhost WebRTC.

Run TWO instances of this program on the same PC:

  terminal 1:  python mic_link.py          -> click "Microphone Input (host)"
  terminal 2:  python mic_link.py          -> click "Microphone Output (connect)"

The Input instance listens on 127.0.0.1:8700, captures the default microphone
and streams it over WebRTC. The Output instance auto-connects to that socket,
does the WebRTC handshake automatically and plays the incoming mic audio on
the default speakers. No SDP copy/paste needed.

Notes:
  - The Input instance mutes its own playback and the Output instance mutes
    its own mic, so there is no feedback/echo loop on the shared machine.
  - Optional: --port <n> to use a different signaling port.

Headless mode (for testing / debugging, no GUI):
  terminal 1:  python mic_link.py --headless host
  terminal 2:  python mic_link.py --headless guest
"""
import ctypes
import json
import os
import queue
import socket
import sys
import threading
import time

try:
    import tkinter as tk
    from tkinter import scrolledtext
    HAVE_TK = True
except Exception:
    HAVE_TK = False

DLL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist", "x64", "webrtc_api.dll")
HOST = "127.0.0.1"
PORT = 8700

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
dll.net_create_offer.argtypes = [ctypes.c_double]
dll.net_create_answer.restype = ctypes.c_double
dll.net_create_answer.argtypes = [ctypes.c_double]
dll.net_set_remote.restype = ctypes.c_double
dll.net_set_remote.argtypes = [ctypes.c_double, ctypes.c_char_p, ctypes.c_char_p]
dll.net_add_ice.restype = ctypes.c_double
dll.net_add_ice.argtypes = [ctypes.c_double, ctypes.c_char_p, ctypes.c_double, ctypes.c_char_p]
dll.net_voice.restype = ctypes.c_double
dll.net_voice.argtypes = [ctypes.c_double, ctypes.c_double]
dll.net_close_pc.restype = None
dll.net_close_pc.argtypes = [ctypes.c_double]
dll.net_audio_volume.restype = ctypes.c_double
dll.net_audio_volume.argtypes = [ctypes.c_double]
dll.net_audio_mute.restype = ctypes.c_double
dll.net_audio_mute.argtypes = [ctypes.c_double]
dll.net_poll.restype = ctypes.c_double
dll.net_event_string.restype = ctypes.c_char_p
dll.net_event_string2.restype = ctypes.c_char_p
dll.net_event_int.restype = ctypes.c_double

STATE_NAMES = {
    0: "new", 1: "connecting", 2: "connected", 3: "disconnected",
    4: "failed", 5: "closed",
}


class MicLink:
    def __init__(self, role, port, log):
        self.role = role            # "host" (input) or "guest" (output)
        self.port = port
        self.log = log              # callable(msg)
        self.pc = None              # pc id from net_create_pc
        self.sock = None
        self.connected = False
        self.pending = []           # host: events buffered until client connects
        self.ready = threading.Event()
        self.stop = threading.Event()

    def logf(self, msg):
        if self.log:
            self.log(msg)
        else:
            print(msg, flush=True)

    # ---------- signaling wire ----------
    def send_json(self, obj):
        if not self.sock:
            return
        try:
            self.sock.sendall((json.dumps(obj) + "\n").encode())
        except OSError as e:
            self.logf(f"[sig] send failed: {e}")

    # ---------- roles ----------
    def start_host(self):
        """Microphone input side: listen, capture mic, send offer."""
        dll.net_init()
        dll.net_add_ice_server(b"stun:stun.l.google.com:19302")
        self.pc = dll.net_create_pc()
        dll.net_voice(self.pc, 1.0)   # mic capture on
        dll.net_audio_volume(0.0)     # mute own playback (no echo on shared machine)
        dll.net_create_offer(self.pc)
        self.logf("[host] mic input ready, waiting for output side on "
                  f"127.0.0.1:{self.port} ...")

        def accept():
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((HOST, self.port))
            srv.listen(1)
            conn, addr = srv.accept()
            self.sock = conn
            self.connected = True
            self.logf(f"[host] output side connected from {addr}")
            with threading.Lock():
                for obj in self.pending:      # offer + any early ICE
                    self.send_json(obj)
                self.pending.clear()
            self.ready.set()
            srv.close()

        threading.Thread(target=accept, daemon=True).start()
        threading.Thread(target=self.reader_loop, daemon=True).start()
        threading.Thread(target=self.poller_loop, daemon=True).start()

    def start_guest(self):
        """Microphone output side: connect, answer offer, play audio."""
        dll.net_init()
        dll.net_add_ice_server(b"stun:stun.l.google.com:19302")
        self.pc = dll.net_create_pc()
        dll.net_voice(self.pc, 1.0)   # playback on
        dll.net_audio_mute(1.0)       # mute own mic (input side sends the audio)
        self.logf(f"[guest] connecting to 127.0.0.1:{self.port} ...")

        for attempt in range(100):
            try:
                self.sock = socket.create_connection((HOST, self.port), timeout=3)
                self.connected = True
                break
            except OSError:
                if attempt == 0:
                    self.logf("[guest] waiting for input side to start...")
                time.sleep(0.5)
        if not self.connected:
            self.logf("[guest] could not connect - is the Input instance running?")
            return
        self.logf("[guest] connected to input side")
        self.ready.set()
        threading.Thread(target=self.reader_loop, daemon=True).start()
        threading.Thread(target=self.poller_loop, daemon=True).start()

    # ---------- background loops ----------
    def reader_loop(self):
        """Read signaling messages from the peer and feed them to the DLL."""
        self.ready.wait()  # wait until the signaling socket exists
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    m = json.loads(line.decode())
                except ValueError:
                    continue
                try:
                    if m["type"] == "sdp":
                        dll.net_set_remote(self.pc, m["sdp"].encode(), m["sdpType"].encode())
                        if self.role == "guest" and m["sdpType"] == "offer":
                            dll.net_create_answer(self.pc)
                            self.logf("[guest] got offer, sent answer")
                    elif m["type"] == "ice":
                        dll.net_add_ice(self.pc, m.get("mid", "").encode(), 0.0,
                                        m["cand"].encode())
                except Exception as e:
                    self.logf(f"[sig] error handling message: {e}")
        self.logf("[sig] signaling connection closed")

    def poller_loop(self):
        """Poll DLL events; send SDP/ICE to peer, log state changes."""
        last_state = None
        while not self.stop.is_set():
            t = dll.net_poll()
            if t == -1:
                time.sleep(0.005)
                continue
            if t == 1:  # local SDP
                sdp = dll.net_event_string().decode()
                st = dll.net_event_string2().decode()
                obj = {"type": "sdp", "sdp": sdp, "sdpType": st}
                if self.connected:
                    self.send_json(obj)
                    self.logf(f"[sig] sent {st}")
                else:
                    with threading.Lock():
                        self.pending.append(obj)
            elif t == 2:  # local ICE
                obj = {"type": "ice", "mid": dll.net_event_string2().decode(),
                       "cand": dll.net_event_string().decode()}
                if self.connected:
                    self.send_json(obj)
                else:
                    with threading.Lock():
                        self.pending.append(obj)
            elif t == 8:  # connection state
                st = int(dll.net_event_int())
                name = STATE_NAMES.get(st, str(st))
                if st != last_state:
                    last_state = st
                    self.logf(f"[rtc] state: {name}")
                    if st == 2:
                        self.logf("[rtc] CONNECTED - audio link is live")
                    elif st in (3, 4, 5):
                        self.logf(f"[rtc] link ended ({name})")
            elif t == 11:
                self.logf("[audio] track open")
            elif t == 12:
                self.logf("[audio] track closed")
            elif t == 9:
                self.logf(f"[error] {dll.net_event_string().decode()}")

    def shutdown(self):
        self.stop.set()
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        try:
            if self.pc is not None:
                dll.net_voice(self.pc, 0.0)
                dll.net_close_pc(self.pc)
        except Exception:
            pass
        try:
            dll.net_terminate()
        except Exception:
            pass


# ---------------- GUI ----------------
if HAVE_TK:

    class App:
        def __init__(self, port):
            self.root = tk.Tk()
            self.root.title("Mic Link - choose a side")
            self.root.geometry("460x330")
            self.port = port
            self.link = None
            self.q = queue.Queue()

            frm = tk.Frame(self.root, padx=12, pady=12)
            frm.pack(fill="both", expand=True)

            tk.Label(frm, text="Run one instance on each side of the link:",
                     anchor="w").pack(fill="x")
            self.btn_in = tk.Button(
                frm, text="1.  Microphone Input  (host - listens on this PC)",
                command=lambda: self.pick("host"), bg="#d9f2d9",
                font=("Segoe UI", 11, "bold"), pady=8)
            self.btn_in.pack(fill="x", pady=(8, 4))
            self.btn_out = tk.Button(
                frm, text="2.  Microphone Output  (connect - plays audio)",
                command=lambda: self.pick("guest"), bg="#d9e9f2",
                font=("Segoe UI", 11, "bold"), pady=8)
            self.btn_out.pack(fill="x", pady=(0, 8))

            self.txt = scrolledtext.ScrolledText(frm, height=10, state="disabled",
                                                 font=("Consolas", 9))
            self.txt.pack(fill="both", expand=True)

            tk.Button(frm, text="Quit", command=self.root.destroy).pack(pady=(6, 0))

            self.root.after(100, self.drain)
            self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        def log(self, msg):
            self.q.put(msg)

        def drain(self):
            try:
                while True:
                    msg = self.q.get_nowait()
                    self.txt.configure(state="normal")
                    self.txt.insert("end", msg + "\n")
                    self.txt.see("end")
                    self.txt.configure(state="disabled")
            except queue.Empty:
                pass
            self.root.after(100, self.drain)

        def pick(self, role):
            self.btn_in.configure(state="disabled")
            self.btn_out.configure(state="disabled")
            if role == "host":
                self.root.title("Mic Link - Microphone INPUT (host)")
                self.link = MicLink("host", self.port, self.log)
                self.link.start_host()
            else:
                self.root.title("Mic Link - Microphone OUTPUT (guest)")
                self.link = MicLink("guest", self.port, self.log)
                self.link.start_guest()

        def on_close(self):
            if self.link:
                self.link.shutdown()
            self.root.destroy()

        def run(self):
            self.root.mainloop()


def main():
    port = PORT
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])

    if "--headless" in sys.argv:
        role = sys.argv[sys.argv.index("--headless") + 1]
        link = MicLink(role, port, None)
        (link.start_host if role == "host" else link.start_guest)()

        dll.net_audio_diag = getattr(dll, "net_audio_diag", None)
        if dll.net_audio_diag:
            dll.net_audio_diag.restype = ctypes.c_double
            dll.net_audio_diag.argtypes = [ctypes.c_int]

        print(f"[headless] {role} running on port {port} - Ctrl+C to quit", flush=True)
        try:
            while True:
                if dll.net_audio_diag:
                    n0 = dll.net_audio_diag(0)
                    n1 = dll.net_audio_diag(1)
                    n2 = dll.net_audio_diag(2)
                    n4 = dll.net_audio_diag(6)
                    n5 = dll.net_audio_diag(7)
                    n6 = dll.net_audio_diag(8)
                    n7 = dll.net_audio_diag(9)
                    n8 = dll.net_audio_diag(10)
                    n9 = dll.net_audio_diag(11)
                    nA = dll.net_audio_diag(12)
                    nB = dll.net_audio_diag(13)
                    nC = dll.net_audio_diag(14)
                    print(f"[diag] enc={n0:.0f} sent={n1:.0f} dec={n2:.0f} pb={n4:.0f} "
                          f"wrote={n5:.0f} empty={n6:.0f} popped={n7:.0f} "
                          f"onframe={n8:.0f} nodec={n9:.0f} opusbad={nA:.0f} "
                          f"trid={nB:.0f} decs={nC:.0f}", flush=True)
                time.sleep(1)
        except KeyboardInterrupt:
            link.shutdown()
            print("bye", flush=True)
        return

    if not HAVE_TK:
        print("tkinter not available; use --headless host or --headless guest")
        sys.exit(1)
    App(port).run()


if __name__ == "__main__":
    main()
