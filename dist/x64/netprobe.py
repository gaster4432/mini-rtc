"""net_video_send path probe: watches transport counters while frames flow."""
import ctypes, time, importlib.util
spec = importlib.util.spec_from_file_location("d", "screen720.py")
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)

net = d.Net(d.os.path.join(d.HERE, "webrtc_api.dll"))
net.net_init(); net.net_set_log_level(1.0)
W, H, FPS = 1280, 720, 30
rx = [0]
def on_frame(pc, w, h, st, fmt, data, ts, u):
    rx[0] += 1
cb = d.FRAME_CB(on_frame)
a, b = net.net_create_pc(), net.net_create_pc()
net.net_video_enable(a, W, H, FPS, 2500); net.net_video_enable(b, W, H, FPS, 2500)
net.net_video_set_params(a, W, H, FPS, 2500, 1.0)
net.net_video_set_cb(b, cb, 0.0)
net.net_create_offer(a)
go = ga = False; dl = time.time() + 20
while time.time() < dl:
    t = net.net_poll()
    if t == -1.0: time.sleep(0.005); continue
    s = int(net.net_event_pc()); dst = b if s == a else a
    if t == 1.0:
        sdp = net.net_event_string() or b""; typ = net.net_event_string2() or b""
        if s == a and not go:
            if net.net_set_remote(b, sdp, typ) != 0.0: net.net_create_answer(b); go = True
        elif s == b and not ga:
            if net.net_set_remote(a, sdp, typ) != 0.0: ga = True
    elif t == 2.0:
        c = net.net_event_string() or b""; m = net.net_event_string2() or b""
        if c: net.net_add_ice(dst, m, 0.0, c)
    elif t == 8.0 and int(net.net_event_int()) == 2: break
print("connected\n", flush=True)

buf = (ctypes.c_ubyte * (W * H * 4))()
for i in range(len(buf)): buf[i] = (i * 7) & 255
addr = ctypes.addressof(buf)

hdr = "%4s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s"
print(hdr % ("t","up","enc","sent","auIn","pics","decErr","pli","dropIn","dropOut","cb"), flush=True)
def row(t0):
    g = net.net_video_diag
    print(hdr % (time.time()-t0, g(20), g(1), g(2), g(3), g(4), g(5), g(6), g(16), g(17), rx[0]), flush=True)
fs = 1.0 / FPS; t0 = time.time(); nxt = t0
for k in range(20):
    for _ in range(FPS):
        net.net_video_send(a, addr, W, H, W*4, 2, 0.0)
        nxt += fs; dt = nxt - time.time()
        if dt > 0: time.sleep(dt)
        else: nxt = time.time()
    row(t0)
net.net_close_pc(a); net.net_close_pc(b); net.net_terminate()
print("done", flush=True)
