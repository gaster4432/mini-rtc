"""Ask the DLL which video codec it actually used.

The hardware path is an optimisation with a software fallback, so passing tests
say nothing about whether the GPU was involved. This drives the real send path
and reports what net_video_hw_info says.
"""
import ctypes
import os
import sys
import time

W = int(sys.argv[1]) if len(sys.argv) > 1 else 640
H = int(sys.argv[2]) if len(sys.argv) > 2 else 360
FRAMES = int(sys.argv[3]) if len(sys.argv) > 3 else 40

dll = os.path.join(os.path.dirname(os.path.abspath(__file__)), "webrtc_api.dll")
d = ctypes.CDLL(dll)

CD = ctypes.c_double
# restype matters: without it ctypes defaults to c_int and misreads a double
# returned in XMM0, which is how a working hardware path reported "2".
for _n in ("net_init", "net_create_pc", "net_video_enable", "net_video_set_params",
           "net_video_send", "net_video_keyframe", "net_video_hw_info", "net_video_stats"):
    getattr(d, _n).restype = CD
# argtypes must match each export's real signature: net_init, net_create_pc and
# net_terminate take no arguments at all; net_video_send's second argument is a
# pointer; net_video_hw_info writes a string buffer.
d.net_init.argtypes = []
d.net_create_pc.argtypes = []
d.net_terminate.argtypes = []
d.net_terminate.restype = None
d.net_video_enable.argtypes = [CD] * 5
d.net_video_set_params.argtypes = [CD] * 6
d.net_video_send.argtypes = [CD, ctypes.c_void_p, CD, CD, CD, CD, CD]
d.net_video_keyframe.argtypes = [CD]
d.net_video_hw_info.argtypes = [CD, ctypes.c_char_p, CD]
d.net_video_stats.argtypes = [CD, CD]

print("net_init", d.net_init())
a = d.net_create_pc()
b = d.net_create_pc()
print("peers", a, b)
print("video_enable", d.net_video_enable(a, 0.0, 0.0, 0.0, 0.0),
      d.net_video_enable(b, 0.0, 0.0, 0.0, 0.0))
# pc, w, h, fps, kbps, gopSec
d.net_video_set_params(a, float(W), float(H), 30.0, 2500.0, 1.0)

buf = (ctypes.c_ubyte * (W * H * 3 // 2))()
addr = ctypes.addressof(buf)
sent = 0
for f in range(FRAMES):
    for j in range(H):
        row = j * W
        for i in range(0, W, 997):
            buf[row + i] = (i * 3 + j * 5 + f * 11) & 0xFF
    for i in range(W * H, W * H * 3 // 2):
        buf[i] = 128
    if f == 0:
        d.net_video_keyframe(a)
    if d.net_video_send(a, addr, float(W), float(H), float(W), 2.0, 0.0) != 0.0:
        sent += 1
    time.sleep(1.0 / 30.0)

info = ctypes.create_string_buffer(256)
v = d.net_video_hw_info(a, info, 256.0)
print("sent", sent, "encoded", d.net_video_stats(a, 2.0))
print("hw_info =", v, "->", info.value.decode(errors="replace"))
print("mean encode %.2f ms" % d.net_video_stats(a, 5.0))
d.net_terminate()
print("RESULT:", "HARDWARE PATH ACTIVE" if v == 1.0 else "software path in use")
