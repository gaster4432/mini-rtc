"""vselftest.py - run the video loopback self-test at a chosen resolution.

    python vselftest.py [width height fps frames]
    defaults: 320 240 30 90
"""
import ctypes, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "dist", "x64", "webrtc_api.dll")

W = int(sys.argv[1]) if len(sys.argv) > 1 else 320
H = int(sys.argv[2]) if len(sys.argv) > 2 else 240
FPS = int(sys.argv[3]) if len(sys.argv) > 3 else 30
NFR = int(sys.argv[4]) if len(sys.argv) > 4 else 90
DUMP = os.path.join(HERE, "selftest_%dx%d.264" % (W, H))

BITS = [
    (1 << 0, "SDP contained m=video"),
    (1 << 1, "video track(s) opened"),
    (1 << 2, "ICE connected"),
    (1 << 3, "depacketizer produced access units"),
    (1 << 4, "decoder produced pictures"),
    (1 << 5, "frames intact (size + not blank + not corrupted)"),
    (1 << 6, "IDR produced (forced)"),
]

d = ctypes.CDLL(DLL)
d.net_init.restype = ctypes.c_double
d.net_version.restype = ctypes.c_double
d.net_video_selftest_ex.restype = ctypes.c_double
d.net_video_selftest_ex.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_double,
                                    ctypes.c_double, ctypes.c_char_p]
d.net_video_diag.restype = ctypes.c_double
d.net_video_diag.argtypes = [ctypes.c_int]
d.net_terminate.restype = None
d.net_set_log_level.restype = ctypes.c_double
d.net_set_log_level.argtypes = [ctypes.c_double]

print("=== video self-test: %dx%d @ %d fps, %d frames ===" % (W, H, FPS, NFR))
d.net_init()
import os
if os.environ.get('VTLOG'):
    d.net_set_log_level(3.0)
    print('  (libdatachannel log level 3 = debug)')
t0 = time.time()
mask = int(d.net_video_selftest_ex(float(W), float(H), float(FPS), float(NFR), DUMP.encode()))
dt = time.time() - t0

print("\nmask 0x%02X in %.1fs\n" % (mask, dt))
for bit, name in BITS:
    print("  [%s] %s" % ("PASS" if mask & bit else "FAIL", name))

encoded = d.net_video_diag(0)
sent = d.net_video_diag(1)
idrs = d.net_video_diag(2)
rx = d.net_video_diag(3)
pics = d.net_video_diag(4)
errs = d.net_video_diag(5)
compared = d.net_video_diag(9)
mismatch = d.net_video_diag(10)
baddim = d.net_video_diag(11)
dev = d.net_video_diag(12)
encms = d.net_video_diag(13)
kb = d.net_video_diag(14)

print("\npipeline:")
print("  encoded %d -> sent %d -> access units in %d -> pictures %d (errs %d)"
      % (encoded, sent, rx, pics, errs))
print("  IDRs %d" % idrs)

print("\nintegrity:")
print("  frames compared      %d" % compared)
print("  mismatched frames    %d" % mismatch)
print("  wrong-size frames    %d" % baddim)
print("  worst mean deviation %.4f / 255" % dev)

print("video path:", "HARDWARE (GPU)" if d.net_video_diag(15) == 1.0 else "OpenH264 (software)")
print("lossless:  dropIn=%d  dropOut=%d  blocked=%d  harnessLate=%d  inFlight=%d repeats=%d submitted=%d stillQueued=%d" % (
    d.net_video_diag(16), d.net_video_diag(17), d.net_video_diag(18),
    d.net_video_diag(19), d.net_video_diag(22), d.net_video_diag(23), d.net_video_diag(24), d.net_video_diag(25)))
print("\nperformance:")

budget = 1000.0 / FPS
print("  worst encode time    %.1f ms  (budget %.1f ms at %d fps)" % (encms, budget, FPS))
print("  realtime at %d fps:  %s" % (FPS, "YES" if encms <= budget else "NO - encoder too slow"))
if encms > 0 and dt > 0:
    print("  total payload        %.0f KB over %.1fs = %.0f kbps avg"
          % (kb, dt, kb * 8.0 / dt))

if os.path.exists(DUMP):
    n = os.path.getsize(DUMP)
    with open(DUMP, "rb") as fh:
        b = fh.read()
    types = {}
    for i in range(len(b) - 4):
        if b[i] == 0 and b[i+1] == 0 and b[i+2] == 0 and b[i+3] == 1:
            t = b[i+4] & 0x1F
            types[t] = types.get(t, 0) + 1
    print("\nbitstream: %d bytes -> %s" % (n, os.path.basename(DUMP)))
    print("  starts with: " + " ".join("%02x" % x for x in b[:8]))
    print("  NAL types  : " + ", ".join("%d:%d" % (t, c) for t, c in sorted(types.items()))
          + "   (7=SPS 8=PPS 5=IDR 1=slice)")

d.net_terminate()
full = all(mask & b for b, _ in BITS)
print("\nRESULT: %s" % ("ALL LAYERS PASS" if full else "SOME LAYERS FAILED"))
sys.exit(0 if full else 1)
