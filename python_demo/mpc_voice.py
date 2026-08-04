import ctypes, time
d = ctypes.CDLL(r'C:\Users\archl\Documents\mini-rtc\dist\x64\webrtc_api.dll')
d.net_init.restype = ctypes.c_double
d.net_create_pc.restype = ctypes.c_double
d.net_voice.restype = ctypes.c_double
d.net_voice.argtypes = [ctypes.c_double, ctypes.c_double]
d.net_audio_volume.restype = ctypes.c_double
d.net_audio_volume.argtypes = [ctypes.c_double]
d.net_terminate.restype = None
d.net_close_pc.restype = None
d.net_close_pc.argtypes = [ctypes.c_double]
print('init', d.net_init(), flush=True)
p1 = d.net_create_pc(); print('p1', p1, flush=True)
p2 = d.net_create_pc(); print('p2', p2, flush=True)
print('v1', d.net_voice(1, 1.0), flush=True)
print('v2', d.net_voice(2, 1.0), flush=True)
d.net_audio_volume(0.0)
time.sleep(2)
print('close p2', flush=True)
d.net_close_pc(2)
time.sleep(1)
print('v1 off', d.net_voice(1, 0.0), flush=True)
d.net_terminate()
print('done', flush=True)