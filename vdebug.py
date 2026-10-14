"""vdebug.py - run the self-test with libdatachannel warnings visible.

    python vdebug.py [w h fps frames]

A dump name beginning with '!' keeps the logger at warning level so that
per-packet transport warnings are visible.
"""
import ctypes, os, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "dist", "x64", "webrtc_api.dll")
W = int(sys.argv[1]) if len(sys.argv) > 1 else 640
H = int(sys.argv[2]) if len(sys.argv) > 2 else 360
FPS = int(sys.argv[3]) if len(sys.argv) > 3 else 60
N = int(sys.argv[4]) if len(sys.argv) > 4 else 60

d = ctypes.CDLL(DLL)
d.net_init.restype = ctypes.c_double
d.net_video_selftest_ex.restype = ctypes.c_double
d.net_video_selftest_ex.argtypes = [ctypes.c_double] * 4 + [ctypes.c_char_p]
d.net_video_diag.restype = ctypes.c_double
d.net_video_diag.argtypes = [ctypes.c_int]
d.net_terminate.restype = None

d.net_init()

# Watchdog: ctypes releases the GIL during the call, so these counters can be
# read while the self-test is still running inside the DLL. That tells us how
# far it got if it ever stops making progress.
stop = threading.Event()


def watch():
    while not stop.wait(2.0):
        try:
            print("  [watch] enc=%d sent=%d rx=%d pics=%d decerr=%d" % (
                d.net_video_diag(0), d.net_video_diag(1), d.net_video_diag(3),
                d.net_video_diag(4), d.net_video_diag(5)), flush=True)
        except Exception as e:
            print("  [watch] error:", e, flush=True)


threading.Thread(target=watch, daemon=True).start()

t0 = time.time()
mask = d.net_video_selftest_ex(float(W), float(H), float(FPS), float(N), b"!vt.264")
dt = time.time() - t0
print("RESULT %dx%d@%d n=%d mask=0x%02X in %.1fs" % (W, H, FPS, N, int(mask), dt))
print("sent=%d rx=%d pics=%d encerr=%d" % (
    d.net_video_diag(1), d.net_video_diag(3), d.net_video_diag(4), d.net_video_diag(5)))
d.net_terminate()
print("done")
