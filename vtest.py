"""vtest.py - run the video self-test and decode the layer bitmask.

    1  the SDP carried an m=video section
    2  both video tracks opened
    4  ICE connected  (bit 3, value 8)
    8  depacketizer reassembled access units
   16  decoder produced pictures
   32  decoded picture is the right size and not blank
   64  a forced IDR was produced
"""
import ctypes, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "dist", "x64", "webrtc_api.dll")

BITS = [
    (1 << 0, "SDP contained m=video"),
    (1 << 1, "video track(s) opened"),
    (1 << 2, "ICE connected"),
    (1 << 3, "depacketizer produced access units"),
    (1 << 4, "decoder produced pictures"),
    (1 << 5, "picture size correct and not blank"),
    (1 << 6, "IDR produced (forced)"),
]

d = ctypes.CDLL(DLL)
d.net_init.restype = ctypes.c_double
d.net_version.restype = ctypes.c_double
d.net_video_selftest.restype = ctypes.c_double
d.net_video_selftest.argtypes = [ctypes.c_double]
d.net_video_diag.restype = ctypes.c_double
d.net_video_diag.argtypes = [ctypes.c_int]
d.net_terminate.restype = None

DIAG = ["encoded", "sent", "IDRs", "rx access units", "pictures decoded",
        "decode errors", "PLIs", "tracks", "best track frames"]

print("webrtc_api.dll v%.1f" % d.net_version())
d.net_init()
t0 = time.time()
mask = int(d.net_video_selftest(0.0))
dt = time.time() - t0

print("\nselftest returned 0x%02X  (%.1fs)\n" % (mask, dt))
for bit, name in BITS:
    print("  [%s] bit %-2d  %s" % ("PASS" if mask & bit else "FAIL", bit.bit_length() - 1, name))

print("\ncounters:")
for i, name in enumerate(DIAG):
    print("  %-22s %d" % (name, d.net_video_diag(i)))

f = os.path.join(HERE, "selftest.264")
if os.path.exists(f):
    n = os.path.getsize(f)
    print("\nselftest.264: %d bytes  (open this in VLC to check the encoder)"
          % n)
    with open(f, "rb") as fh:
        head = fh.read(8)
    print("  first bytes: " + " ".join("%02x" % b for b in head))
    # count NAL start codes
    with open(f, "rb") as fh:
        b = fh.read()
    types = {}
    for i in range(len(b) - 4):
        if b[i] == 0 and b[i+1] == 0 and b[i+2] == 0 and b[i+3] == 1:
            t = b[i+4] & 0x1F
            types[t] = types.get(t, 0) + 1
    print("  NAL types: " + ", ".join("%d:%d" % (t, c) for t, c in sorted(types.items())))
    print("  (7=SPS 8=PPS 5=IDR 1=non-IDR slice)")

d.net_terminate()
full = all(mask & b for b, _ in BITS)
print("\nRESULT: %s" % ("ALL LAYERS PASS" if full else "SOME LAYERS FAILED"))
sys.exit(0 if full else 1)
