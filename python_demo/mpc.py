import ctypes, time, sys
d = ctypes.CDLL(r'C:\Users\archl\Documents\mini-rtc\dist\x64\webrtc_api.dll')
d.net_init.restype = ctypes.c_double
d.net_create_pc.restype = ctypes.c_double
d.net_pc_count.restype = ctypes.c_double
d.net_terminate.restype = None
print('init', d.net_init(), flush=True)
p1 = d.net_create_pc(); print('p1', p1, flush=True)
p2 = d.net_create_pc(); print('p2', p2, flush=True)
p3 = d.net_create_pc(); print('p3', p3, flush=True)
print('count', d.net_pc_count(), flush=True)
d.net_terminate()
print('done', flush=True)