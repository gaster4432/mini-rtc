// screen720.cpp - mini-rtc screen-share demo.
//
// Captures the primary display scaled to 1280x720, sends it over a real
// WebRTC peer connection (loopback, in-process), and displays the frames that
// come back in a window. The capture and the display both live here in the
// demo: the DLL only converts the buffers it is handed, which is the whole
// point of keeping it a library.
//
//   build:  build.bat          (produces dist\x64\webrtc_api.dll first)
//           g++ -O2 -o screen720.exe screen720.cpp -Idist/x64 \
//               -Ldist/x64 -lwebrtc_api
//   run:    screen720.exe [--fps 30] [--bitrate 2500] [--no-show]
//
// Esc or close the window to quit.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef double(__cdecl *net_poll_fn)(void);
typedef double(__cdecl *net_event_pc_fn)(void);
typedef double(__cdecl *net_event_int_fn)(void);
typedef const char *(__cdecl *net_event_string_fn)(void);
typedef const char *(__cdecl *net_event_string2_fn)(void);
typedef double(__cdecl *net_init_fn)(void);
typedef void(__cdecl *net_terminate_fn)(void);
typedef double(__cdecl *net_create_pc_fn)(void);
typedef void(__cdecl *net_close_pc_fn)(double);
typedef double(__cdecl *net_create_offer_fn)(double);
typedef double(__cdecl *net_create_answer_fn)(double);
typedef double(__cdecl *net_set_remote_fn)(double, const char *, const char *);
typedef double(__cdecl *net_add_ice_fn)(double, const char *, double, const char *);
typedef double(__cdecl *net_video_enable_fn)(double, double, double, double, double);
typedef double(__cdecl *net_video_set_params_fn)(double, double, double, double, double, double);
typedef double(__cdecl *net_video_send_fn)(double, int64_t, double, double, double, double, double);
typedef double(__cdecl *net_video_keyframe_fn)(double);
typedef double(__cdecl *net_video_set_cb_fn)(double, void *, double);
typedef double(__cdecl *net_video_stats_fn)(double, double);
typedef double(__cdecl *net_set_log_level_fn)(double);

// The DLL calls this on its own thread when a frame has been decoded.
static void __cdecl onFrame(int pc, int w, int h, int stride, int format, const void *data,
                            double tsMs, double user) {
  if (format != 2 || !data || w <= 0 || h <= 0) return; // 2 = BGRA
  // Hand the pixels to the window. Copying is fine: the window paints from a
  // DIB and this only runs at the incoming frame rate.
  extern void presentFrame(const void *bgra, int w, int h, int stride);
  presentFrame(data, w, h, stride);
}

static HWND g_win = NULL;
static std::vector<unsigned char> g_frame; // BGRA, doubles as the DIB backing store
static int g_fw = 0, g_fh = 0;
static CRITICAL_SECTION g_cs;
static bool g_haveFrame = false;
static volatile LONG g_framesPainted = 0;

void presentFrame(const void *bgra, int w, int h, int stride) {
  EnterCriticalSection(&g_cs);
  if ((int)g_frame.size() != w * h * 4) g_frame.resize((size_t)w * h * 4);
  if (stride == w * 4) {
    memcpy(g_frame.data(), bgra, (size_t)w * h * 4);
  } else {
    const unsigned char *s = (const unsigned char *)bgra;
    for (int j = 0; j < h; j++)
      memcpy(&g_frame[(size_t)j * w * 4], s + (size_t)j * stride, (size_t)w * 4);
  }
  g_fw = w;
  g_fh = h;
  g_haveFrame = true;
  InterlockedIncrement(&g_framesPainted);
  LeaveCriticalSection(&g_cs);
  if (g_win) PostMessage(g_win, WM_APP + 1, 0, 0); // wake the window to repaint
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_APP + 1:
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    EnterCriticalSection(&g_cs);
    if (g_haveFrame && g_fw > 0) {
      BITMAPINFO bi;
      memset(&bi, 0, sizeof(bi));
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = g_fw;
      bi.bmiHeader.biHeight = -g_fh; // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      // fit the frame into the window, keeping the aspect ratio
      int dw = rc.right, dh = rc.bottom;
      double s = (double)dw / g_fw;
      if ((double)dh / g_fh < s) s = (double)dh / g_fh;
      int pw = (int)(g_fw * s), ph = (int)(g_fh * s);
      int ox = (dw - pw) / 2, oy = (dh - ph) / 2;
      SetStretchBltMode(dc, COLORONCOLOR);
      StretchDIBits(dc, ox, oy, pw, ph, 0, 0, g_fw, g_fh, g_frame.data(), &bi, DIB_RGB_COLORS,
                    SRCCOPY);
    }
    LeaveCriticalSection(&g_cs);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) DestroyWindow(h);
    return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

int main(int argc, char **argv) {
  int fps = 30, kbps = 2500;
  bool show = true;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) kbps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-show")) show = false;
  }
  const int W = 1280, H = 720; // 720p

  HMODULE dll = LoadLibraryA("webrtc_api.dll");
  if (!dll) {
    printf("could not load webrtc_api.dll (%lu) - keep it next to this exe\n", GetLastError());
    return 1;
  }
  auto G = [&](const char *n) { return (void *)GetProcAddress(dll, n); };
  net_init_fn net_init = (net_init_fn)G("net_init");
  net_terminate_fn net_terminate = (net_terminate_fn)G("net_terminate");
  net_create_pc_fn net_create_pc = (net_create_pc_fn)G("net_create_pc");
  net_close_pc_fn net_close_pc = (net_close_pc_fn)G("net_close_pc");
  net_create_offer_fn net_create_offer = (net_create_offer_fn)G("net_create_offer");
  net_create_answer_fn net_create_answer = (net_create_answer_fn)G("net_create_answer");
  net_set_remote_fn net_set_remote = (net_set_remote_fn)G("net_set_remote");
  net_add_ice_fn net_add_ice = (net_add_ice_fn)G("net_add_ice");
  net_poll_fn net_poll = (net_poll_fn)G("net_poll");
  net_event_pc_fn net_event_pc = (net_event_pc_fn)G("net_event_pc");
  net_event_int_fn net_event_int = (net_event_int_fn)G("net_event_int");
  net_event_string_fn net_event_string = (net_event_string_fn)G("net_event_string");
  net_event_string2_fn net_event_string2 = (net_event_string2_fn)G("net_event_string2");
  net_video_enable_fn net_video_enable = (net_video_enable_fn)G("net_video_enable");
  net_video_set_params_fn net_video_set_params = (net_video_set_params_fn)G("net_video_set_params");
  net_video_send_fn net_video_send = (net_video_send_fn)G("net_video_send");
  net_video_keyframe_fn net_video_keyframe = (net_video_keyframe_fn)G("net_video_keyframe");
  net_video_set_cb_fn net_video_set_cb = (net_video_set_cb_fn)G("net_video_set_cb");
  net_video_stats_fn net_video_stats = (net_video_stats_fn)G("net_video_stats");
  net_set_log_level_fn net_set_log_level = (net_set_log_level_fn)G("net_set_log_level");
  if (!net_init || !net_create_pc || !net_video_send || !net_video_set_cb) {
    printf("the DLL is missing expected exports - rebuild with build.bat\n");
    return 1;
  }
  net_set_log_level(1.0); // errors only; libdatachannel is chatty at warning level

  if (show) {
    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.lpszClassName = "mini-rtc screen demo";
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wc);
    g_win = CreateWindowEx(0, "mini-rtc screen demo", "mini-rtc - received screen (720p)",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, W + 40, H + 60, NULL,
                           NULL, wc.hInstance, NULL);
    ShowWindow(g_win, SW_SHOW);
    // Keep our own window out of the screen capture. Without this the demo
    // captures its own output, draws it inside itself, recaptures that, and so
    // on: an infinite mirror whose rescaled title bar degrades into noise.
    // Windows renders an excluded window black in captures.
    if (!SetWindowDisplayAffinity(g_win, WDA_EXCLUDEFROMCAPTURE)) {
      printf("note: could not exclude this window from capture (needs Windows 10 2004+);\n"
             "      move the window off-screen if the picture looks recursive\n");
    }
  }
  InitializeCriticalSection(&g_cs);

  net_init();
  const double pcA = net_create_pc(); // sender
  const double pcB = net_create_pc(); // receiver
  printf("pc ids: sender=%g receiver=%g\n", pcA, pcB);

  // Both tracks must exist before the offer or video never reaches the SDP.
  net_video_enable(pcA, W, H, fps, kbps);
  net_video_enable(pcB, W, H, fps, kbps);
  net_video_set_params(pcA, W, H, fps, kbps, 1.0);
  net_video_set_cb(pcB, (void *)onFrame, 0.0);

  // ---- loopback signaling: wire the two peers to each other ----
  net_create_offer(pcA);
  bool gotOffer = false, gotAnswer = false, connected = false;
  const ULONGLONG sigDeadline = GetTickCount64() + 20000;
  while (GetTickCount64() < sigDeadline) {
    const double t = net_poll();
    if (t == -1.0) { Sleep(5); continue; }
    const int src = (int)net_event_pc();
    const int dst = (src == (int)pcA) ? (int)pcB : (int)pcA;
    if (t == 1.0) {
      std::string s = net_event_string() ? net_event_string() : "";
      std::string ty = net_event_string2() ? net_event_string2() : "";
      if (src == (int)pcA && !gotOffer) {
        if (net_set_remote(pcB, s.c_str(), ty.c_str()) != 0.0) { net_create_answer(pcB); gotOffer = true; }
      } else if (src == (int)pcB && !gotAnswer) {
        if (net_set_remote(pcA, s.c_str(), ty.c_str()) != 0.0) gotAnswer = true;
      }
    } else if (t == 2.0) {
      std::string c = net_event_string() ? net_event_string() : "";
      std::string m = net_event_string2() ? net_event_string2() : "";
      if (!c.empty()) net_add_ice(dst, m.c_str(), 0, c.c_str());
    } else if (t == 8.0) {
      if ((int)net_event_int() == 2) { connected = true; break; }
    }
  }
  if (!connected) {
    printf("could not connect the two peers\n");
    net_terminate();
    return 1;
  }
  printf("connected - capturing %dx%d @ %d fps, %d kbps\n", W, H, fps, kbps);

  // ---- screen capture target: the primary monitor ----
  HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfo(mon, &mi);
  const int sw = mi.rcMonitor.right - mi.rcMonitor.left;
  const int sh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  printf("source display: %dx%d -> scaled to %dx%d\n", sw, sh, W, H);

  HDC screen = GetDC(NULL);
  HDC memDC = CreateCompatibleDC(screen);
  BITMAPINFO bmi;
  memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = W;
  bmi.bmiHeader.biHeight = -H;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  SelectObject(memDC, bmp);

  const ULONGLONG frameMs = 1000 / (fps > 0 ? fps : 30);
  ULONGLONG next = GetTickCount64();
  long sent = 0, keyframes = 0;
  ULONGLONG tStart = GetTickCount64();

  MSG msg;
  bool running = true;
  while (running) {
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running = false;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (!running) break;

    // keep the event queue moving
    for (int i = 0; i < 32 && net_poll() != -1.0;) {
    }
    if (!g_haveFrame) Sleep(1);

    // capture the whole desktop, scaled down to 720p
    SetStretchBltMode(memDC, COLORONCOLOR);
    StretchBlt(memDC, 0, 0, W, H, screen, mi.rcMonitor.left, mi.rcMonitor.top, sw, sh, SRCCOPY);

    // 1 = handed over, 0 = dropped (track not open yet)
    if (net_video_send(pcA, (int64_t)bits, W, H, W * 4, 2 /*BGRA*/, 0) != 0.0) sent++;
    if (sent == 1) { net_video_keyframe(pcA); keyframes++; }

    // stats once a second
    static ULONGLONG lastStat = 0;
    if (GetTickCount64() - lastStat > 1000) {
      lastStat = GetTickCount64();
      const double secs = (double)(GetTickCount64() - tStart) / 1000.0;
      const double sentN = net_video_stats(pcA, 0);
      const double gotN = net_video_stats(pcB, 2);
      const double skipped = net_video_stats(pcB, 3);
      const double encMs = net_video_stats(pcA, 5);
      printf("\r  up %4.0f fps   received %4.0f fps   encode %5.1f ms   skipped %4.0f   window %d   ",
             secs > 0 ? sentN / secs : 0, secs > 0 ? gotN / secs : 0, encMs, skipped,
             (int)g_framesPainted);
      fflush(stdout);
    }

    next += frameMs;
    const ULONGLONG now = GetTickCount64();
    if (next > now) Sleep((DWORD)(next - now));
    else next = now;
  }

  printf("\n\ndone: %ld frames sent\n", sent);
  DeleteObject(bmp);
  DeleteDC(memDC);
  ReleaseDC(NULL, screen);
  DeleteCriticalSection(&g_cs);
  net_video_set_cb(pcB, 0, 0);
  net_close_pc(pcA);
  net_close_pc(pcB);
  net_terminate();
  FreeLibrary(dll);
  return 0;
}
