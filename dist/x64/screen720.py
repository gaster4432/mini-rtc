"""screen720.py - mini-rtc screen-share demo in Python.

Captures your primary display scaled to 1280x720, sends it over a real WebRTC
peer connection (loopback, in-process), and shows the frames that come back.

The capture and the display both live here in the demo. The DLL only converts
the buffers it is handed, which is what keeps it a library rather than a
screen-sharing app.

    python screen720.py                  # 720p30 preview window
    python screen720.py --fps 60
    python screen720.py --bitrate 6000
    python screen720.py --headless       # no window, stats only
    python screen720.py --no-mirror      # do not blank our own window in the capture

Needs webrtc_api.dll next to this file (that is dist\\x64). Ctrl+C or close the
window to quit.
"""
import ctypes
from ctypes import wintypes
import argparse
import os
import queue
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.join(HERE, "webrtc_api.dll")

W, H = 1280, 720            # 720p
FMT_BGRA = 2
PREVIEW_W, PREVIEW_H = 900, 506

# --------------------------------------------------------------------------
# ctypes bindings for the DLL
# --------------------------------------------------------------------------
FRAME_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                            ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                            ctypes.c_double, ctypes.c_double)


class Net:
    def __init__(self, path):
        self.d = ctypes.CDLL(path)
        d = self.d
        for fn in ("net_init", "net_create_pc", "net_create_offer", "net_create_answer",
                   "net_set_remote", "net_add_ice", "net_poll", "net_event_pc",
                   "net_event_int", "net_video_enable", "net_video_set_params",
                   "net_video_send", "net_video_keyframe", "net_video_set_cb",
                   "net_video_stats", "net_set_log_level", "net_video_selftest"):
            getattr(d, fn).restype = ctypes.c_double
        d.net_init.restype = ctypes.c_double
        d.net_event_string.restype = ctypes.c_char_p
        d.net_event_string2.restype = ctypes.c_char_p
        d.net_video_send.argtypes = [ctypes.c_double, ctypes.c_int64, ctypes.c_double,
                                     ctypes.c_double, ctypes.c_double, ctypes.c_double,
                                     ctypes.c_double]
        d.net_video_set_cb.argtypes = [ctypes.c_double, FRAME_CB, ctypes.c_double]
        d.net_video_set_params.argtypes = [ctypes.c_double] * 6
        d.net_set_remote.argtypes = [ctypes.c_double, ctypes.c_char_p, ctypes.c_char_p]
        d.net_add_ice.argtypes = [ctypes.c_double, ctypes.c_char_p, ctypes.c_double,
                                  ctypes.c_char_p]
        d.net_close_pc.argtypes = [ctypes.c_double]
        d.net_create_offer.argtypes = [ctypes.c_double]
        d.net_create_answer.argtypes = [ctypes.c_double]
        d.net_video_enable.argtypes = [ctypes.c_double] * 5
        d.net_video_keyframe.argtypes = [ctypes.c_double]
        d.net_video_stats.argtypes = [ctypes.c_double, ctypes.c_double]
        d.net_set_log_level.argtypes = [ctypes.c_double]
        d.net_init.argtypes = []
        d.net_create_pc.argtypes = []
        d.net_poll.argtypes = []
        d.net_event_pc.argtypes = []
        d.net_event_int.argtypes = []
        d.net_event_string.argtypes = []
        d.net_event_string2.argtypes = []
        d.net_terminate.restype = None

    def __getattr__(self, name):
        return getattr(self.d, name)


# --------------------------------------------------------------------------
# screen capture (GDI, no third-party deps)
# --------------------------------------------------------------------------
u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


u32.GetSystemMetrics.restype = ctypes.c_int
u32.GetDC.restype = wintypes.HDC
u32.GetDC.argtypes = [wintypes.HWND]
u32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
g32.CreateCompatibleDC.restype = wintypes.HDC
g32.CreateCompatibleDC.argtypes = [wintypes.HDC]
g32.CreateDIBSection.restype = wintypes.HBITMAP
g32.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.POINTER(BITMAPINFO), wintypes.UINT,
                                  ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
g32.SelectObject.restype = wintypes.HGDIOBJ
g32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
# These take handles, and without argtypes ctypes assumes c_int and truncates a
# 64-bit handle - which surfaces as "int too long to convert" during shutdown,
# long after anything useful has been printed.
g32.DeleteDC.argtypes = [wintypes.HDC]
g32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
g32.GetStockObject.restype = wintypes.HGDIOBJ
g32.GetStockObject.argtypes = [ctypes.c_int]
g32.SetStretchBltMode.argtypes = [wintypes.HDC, wintypes.UINT]
g32.StretchBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                           wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                           wintypes.DWORD]

SRCCOPY = 0x00CC0020
COLORONCOLOR = 4


class ScreenCapture:
    """Grabs the primary desktop, scaled to w x h, into a reusable DIB."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.sw = u32.GetSystemMetrics(0)
        self.sh = u32.GetSystemMetrics(1)
        self.hdc = u32.GetDC(None)
        self.mem = g32.CreateCompatibleDC(self.hdc)
        bi = BITMAPINFO()
        bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.bmiHeader.biWidth = w
        bi.bmiHeader.biHeight = -h
        bi.bmiHeader.biPlanes = 1
        bi.bmiHeader.biBitCount = 32
        bi.bmiHeader.biCompression = 0
        self.bits = ctypes.c_void_p()
        self.bmp = g32.CreateDIBSection(self.mem, ctypes.byref(bi), 0,
                                         ctypes.byref(self.bits), None, 0)
        if not self.bmp:
            raise RuntimeError("CreateDIBSection failed (%d)" % ctypes.get_last_error())
        g32.SelectObject(self.mem, self.bmp)
        g32.SetStretchBltMode(self.mem, COLORONCOLOR)

    def grab(self):
        g32.StretchBlt(self.mem, 0, 0, self.w, self.h, self.hdc, 0, 0,
                       self.sw, self.sh, SRCCOPY)
        return (ctypes.c_ubyte * (self.w * self.h * 4)).from_address(self.bits.value)

    def read_buf(self):
        return ctypes.string_at(self.bits, self.w * self.h * 4)

    def blank_rect(self, x, y, w, h):
        """Paint our own preview area black in the captured image.

        Without this the demo captures its own output and redraws it inside
        itself, which recurses. We deliberately do NOT use
        SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): that makes the window
        render solid black in *every* screen capture and screenshot, which is
        confusing. Blanking just our own rectangle keeps the window normal
        everywhere else.
        """
        if w <= 0 or h <= 0:
            return
        # capture is the whole desktop scaled by w/self.sw
        sx = max(0, min(self.w, int(x * self.w / self.sw)))
        sy = max(0, min(self.h, int(y * self.h / self.sh)))
        ex = max(sx, min(self.w, int((x + w) * self.w / self.sw)))
        ey = max(sy, min(self.h, int((y + h) * self.h / self.sh)))
        if ex <= sx or ey <= sy:
            return
        row = b"\x00" * ((ex - sx) * 4)
        addr = self.bits
        for j in range(sy, ey):
            off = (j * self.w + sx) * 4
            ctypes.memmove(ctypes.c_void_p(addr.value + off), row, len(row))

    def close(self):
        u32.ReleaseDC(None, self.hdc)
        g32.SelectObject(self.mem, g32.GetStockObject(0))   # detach before delete
        g32.DeleteDC(self.mem)
        g32.DeleteObject(self.bmp)


def stamp_frame(buf, index, w):
    """Burn a 16-bit frame counter into the top-left 16x16 px as a barcode.

    Lets the receiver identify exactly which frame it is looking at, which is
    how the end-to-end delay below is measured and how we prove the frames that
    arrive are the frames that were sent.
    """
    for i in range(16):
        val = 255 if (index >> i) & 1 else 0
        sq = bytes([val]) * 4
        sx = (i % 4) * 4
        sy = (i // 4) * 4
        for y in range(sy, sy + 4):
            off = (y * w + sx) * 4
            buf[off:off + 16] = sq * 4


def read_stamp(buf, w):
    index = 0
    for i in range(16):
        off = ((i // 4) * 4 * w + (i % 4) * 4) * 4
        if buf[off] > 128:
            index |= (1 << i)
    return index


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bitrate", type=int, default=2500, help="kbps")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--no-mirror", action="store_true",
                    help="do not blank the preview window inside the capture")
    args = ap.parse_args()

    if not os.path.exists(DLL_PATH):
        print("webrtc_api.dll not found next to this script.")
        print("Build it with build.bat, then run this from dist\\x64.")
        return 1

    net = Net(DLL_PATH)
    net.net_init()
    net.net_set_log_level(1.0)   # after net_init, which resets the level

    # ---- GUI (main thread) ----
    root = None
    # maxsize=1: hold only the newest frame. Anything queued here is latency.
    frames = queue.Queue(maxsize=1)
    stats = {"sent": 0, "recv": 0, "skipped": 0, "encms": 0.0, "fps": 0.0,
             "lat": 0.0, "miss": 0, "t0": time.time()}
    send_clock = {}          # frame index -> monotonic send time
    hwnd_ref = [0, 0, 0, 0]  # our window rect in screen coords

    try:
        import tkinter as tk
        from PIL import Image, ImageTk
        have_gui = not args.headless
    except Exception as e:
        print("no GUI available (%s); running headless" % e)
        have_gui = False

    if have_gui:
        root = tk.Tk()
        root.title("mini-rtc - received screen (720p)")
        root.configure(bg="black")
        root.resizable(False, False)
        cv = tk.Canvas(root, width=PREVIEW_W, height=PREVIEW_H, bg="black",
                       highlightthickness=0)
        cv.pack()
        lbl = tk.Label(root, text="starting...", bg="#1a1a1a", fg="#dddddd",
                       font=("Consolas", 9), anchor="w", padx=6, pady=3)
        lbl.pack(fill="x")
        photo = [None]
        # One canvas item, reused for every frame. Calling create_image on each
        # repaint instead stacks a new item every time and never removes the old
        # one: the canvas grows without bound, redraw cost climbs with it, and
        # the frame rate decays steadily the longer the demo runs. The symptom
        # looks like an encoder giving up; it is Tk accumulating widgets.
        img_item = cv.create_image(0, 0, anchor="nw")

    # ---- the DLL calls this from its own thread; just queue, never touch Tk ----
    def on_frame(pc, w, h, stride, fmt, data, ts, user):
        if fmt != FMT_BGRA or not data:
            return
        buf = ctypes.string_at(data, h * stride)
        # measure end to end: this frame was stamped with its send index
        try:
            idx = read_stamp(buf, stride // 4)
            sent_at = send_clock.get(idx)
            if sent_at is not None:
                d = (time.monotonic() - sent_at) * 1000.0
                if 0 <= d < 10000:
                    prev = stats["lat"]
                    stats["lat"] = d if prev == 0.0 else prev * 0.85 + d * 0.15
        except Exception:
            pass
        try:
            frames.put_nowait((buf, w, h, stride))
        except queue.Full:
            try:
                frames.get_nowait()       # drop the oldest: latency over completeness
                frames.put_nowait((buf, w, h, stride))
                stats["skipped"] += 1
            except Exception:
                pass
        stats["recv"] += 1

    cb = FRAME_CB(on_frame)   # keep a reference alive for the DLL's lifetime

    # ---- two peers, loopback ----
    pc_a = net.net_create_pc()
    pc_b = net.net_create_pc()
    print("peers: sender=%d receiver=%d" % (pc_a, pc_b))

    net.net_video_enable(pc_a, W, H, args.fps, args.bitrate)
    net.net_video_enable(pc_b, W, H, args.fps, args.bitrate)
    net.net_video_set_params(pc_a, W, H, args.fps, args.bitrate, 1.0)
    net.net_video_set_cb(pc_b, cb, 0.0)

    net.net_create_offer(pc_a)
    got_offer = got_answer = connected = False
    deadline = time.time() + 20
    while time.time() < deadline:
        t = net.net_poll()
        if t == -1.0:
            time.sleep(0.005)
            continue
        src = int(net.net_event_pc())
        dst = pc_b if src == pc_a else pc_a
        if t == 1.0:
            sdp = net.net_event_string() or b""
            typ = net.net_event_string2() or b""
            if src == pc_a and not got_offer:
                if net.net_set_remote(pc_b, sdp, typ) != 0.0:
                    net.net_create_answer(pc_b)
                    got_offer = True
            elif src == pc_b and not got_answer:
                if net.net_set_remote(pc_a, sdp, typ) != 0.0:
                    got_answer = True
        elif t == 2.0:
            cand = net.net_event_string() or b""
            mid = net.net_event_string2() or b""
            if cand:
                net.net_add_ice(dst, mid, 0.0, cand)
        elif t == 8.0:
            if int(net.net_event_int()) == 2:
                connected = True
                break
    if not connected:
        print("the two peers would not connect")
        return 1
    print("connected: capturing %dx%d @ %d fps, %d kbps" % (W, H, args.fps, args.bitrate))

    cap = ScreenCapture(W, H)
    print("source display: %dx%d" % (cap.sw, cap.sh))

    stop = threading.Event()

    def sender():
        frame_s = 1.0 / max(1, args.fps)
        next_t = time.time()
        n = 0
        while not stop.is_set():
            try:
                while net.net_poll() != -1.0:
                    pass
            except Exception:
                pass
            bits = cap.grab()
            if not args.no_mirror:
                x, y, w, h = hwnd_ref
                if w and h:
                    cap.blank_rect(x, y, w, h)
            stamp_frame(bits, n, W)
            addr = ctypes.addressof(bits)
            if net.net_video_send(pc_a, addr, W, H, W * 4, FMT_BGRA, 0.0) != 0.0:
                send_clock[n] = time.monotonic()
                # keep the clock small; drop entries the receiver is long past
                if len(send_clock) > 240:
                    for k in list(send_clock)[:120]:
                        del send_clock[k]
                n += 1
                stats["sent"] = n
                if n == 1:
                    net.net_video_keyframe(pc_a)
            next_t += frame_s
            dt = next_t - time.time()
            if dt > 0:
                time.sleep(dt)
            else:
                next_t = time.time()

    th = threading.Thread(target=sender, daemon=True)
    th.start()

    if not have_gui:
        t0 = time.time()
        try:
            while not stop.is_set():
                time.sleep(1.0)
                el = time.time() - t0
                print("\r  up %.1f fps  received %.1f fps  encode %.1f ms   " % (
                    stats["sent"] / el, stats["recv"] / el,
                    net.net_video_stats(pc_a, 5)), end="", flush=True)
        except KeyboardInterrupt:
            pass
        print()
    else:
        def pump():
            # drain the queue and repaint; runs on the Tk thread
            w = h = 0
            try:
                buf, w, h, stride = frames.get_nowait()
            except queue.Empty:
                buf = None
            if buf is not None:
                img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", stride, 1)
                img = img.resize((PREVIEW_W, PREVIEW_H), Image.BILINEAR)
                photo[0] = ImageTk.PhotoImage(img)
                cv.itemconfig(img_item, image=photo[0])
            el = time.time() - stats["t0"]
            lbl.config(text="  up %4d f  recv %4d f  %4.1f fps  delay %5.0f ms  "
                            "enc %5.1f ms  drop %d  %dx%d" % (
                stats["sent"], stats["recv"],
                stats["recv"] / el if el > 0 else 0,
                stats["lat"], net.net_video_stats(pc_a, 5),
                stats["skipped"], w, h))
            root.after(10, pump)

        stats["t0"] = time.time()
        root.after(10, pump)

        # Periodic counter line. The on-screen label shows what this demo did;
        # this shows what the library did, which is the only way to tell a demo
        # display problem from a pipeline problem.
        def report():
            g = net.net_video_diag
            el = max(1e-6, time.time() - stats["t0"])
            print("[%5.1fs] up %4d (%.1f/s)  enc %4d  auIn %4d  pics %4d  "
                  "decErr %3d  pli %3d  dropIn %d  dropOut %d  inFlight %d  "
                  "demoRecv %4d  demoDrop %3d"
                  % (el, stats["sent"], stats["sent"] / el, g(1), g(3), g(4),
                     g(5), g(6), g(16), g(17), g(22), stats["recv"],
                     stats["skipped"]), flush=True)
            root.after(2000, report)
        root.after(2000, report)

        def on_move(_evt=None):
            root.update_idletasks()
            hwnd_ref[0] = root.winfo_rootx()
            hwnd_ref[1] = root.winfo_rooty()
            hwnd_ref[2] = root.winfo_width()
            hwnd_ref[3] = root.winfo_height()

        root.bind("<Configure>", on_move)
        on_move()
        try:
            root.mainloop()
        except KeyboardInterrupt:
            pass

    stop.set()
    time.sleep(0.2)
    try:
        net.net_video_set_cb(pc_b, None, 0.0)
        net.net_close_pc(pc_a)
        net.net_close_pc(pc_b)
        net.net_terminate()
    except Exception:
        pass
    cap.close()
    print("sent %d frames" % stats["sent"])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
