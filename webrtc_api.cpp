// webrtc_api.cpp - WebRTC (libdatachannel) wrapper DLL for GameMaker.
// Built with winlibs g++; links libdatachannel-static + juice + usrsctp + OpenSSL statically.
// Exports extern "C" __cdecl functions for GameMaker external_define(dll_cdecl).
// Callbacks from libdatachannel threads push events; GML polls net_poll() each step.
//
// Events (net_poll returns type, -1 if none):
//   1  local SDP ready            (s1=sdp, s2=type)
//   2  local ICE candidate        (s1=cand, s2=mid, n=0)
//   3  data channel open          (s1="open", n=dc id)
//   4  text message               (s1=text, n=size)
//   6  remote data channel        (n=dc id)
//   7  data channel closed        (n=dc id)
//   8  pc connection state change (n=rtcState)
//   9  description error          (s1=error)
//   10 binary message             (n=size, data via net_event_data)

#define RTC_STATIC
#include <rtc/rtc.h>

#include <windows.h>
#include <mutex>
#include <deque>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

// ---------- globals ----------
static std::mutex g_mtx;
static int g_pc = -1;
static int g_dc = -1; // last known data channel (created or received)
static volatile int g_state = 0; // last rtcState seen on the pc
static std::vector<std::string> g_ice_uris;

struct NetEvent {
  int type;
  int id;
  int n;
  std::string s1;
  std::string s2;
  std::vector<char> data;
};
static std::deque<NetEvent> g_events;

static NetEvent g_cur;
static bool g_cur_valid = false;
static char g_strbuf[65536];
static char g_strbuf2[4096];

static void pushEvt(int type, int id, int n, std::string s1, std::string s2) {
  std::lock_guard<std::mutex> lock(g_mtx);
  g_events.push_back(NetEvent{type, id, n, std::move(s1), std::move(s2), {}});
}

static void pushMsg(int id, const char* data, int size) {
  std::lock_guard<std::mutex> lock(g_mtx);
  NetEvent e;
  e.type = 4;
  e.id = id;
  e.n = size;
  e.s1 = std::string(data, size);
  g_events.push_back(std::move(e));
}

static void pushBin(int id, const char* data, int size) {
  std::lock_guard<std::mutex> lock(g_mtx);
  NetEvent e;
  e.type = 10;
  e.id = id;
  e.n = size;
  e.data.assign(data, data + size);
  g_events.push_back(std::move(e));
}

// ---------- callbacks (fired on libdatachannel internal threads) ----------
static void onLocalDesc(int pc, const char* sdp, const char* type, void* ptr) {
  pushEvt(1, pc, 0, sdp ? sdp : "", type ? type : "");
}

static void onLocalCand(int pc, const char* cand, const char* mid, void* ptr) {
  pushEvt(2, pc, 0, cand ? cand : "", mid ? mid : "");
}

static void onStateChange(int pc, rtcState state, void* ptr) {
  g_state = (int)state;
  pushEvt(8, pc, (int)state, "", "");
}

static void onDcOpen(int dc, void* ptr) {
  g_dc = dc;
  pushEvt(3, dc, dc, "open", "");
}

static void onDcClosed(int dc, void* ptr) {
  pushEvt(7, dc, dc, "closed", "");
}

static void onDcError(int dc, const char* error, void* ptr) {
  pushEvt(9, dc, dc, error ? error : "channel error", "");
}

static bool isTextData(const char* m, int size) {
  for (int i = 0; i < size; i++) {
    unsigned char c = (unsigned char)m[i];
    if (c == 0 || c == 0x7F) return false;              // NUL / DEL -> binary
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false; // control chars -> binary
  }
  return true;
}

static void onDcMessage(int dc, const char* message, int size, void* ptr) {
  if (!message || size < 0) return;
  if (isTextData(message, size)) pushMsg(dc, message, size);
  else pushBin(dc, message, size);
}

static void onDataChannel(int pc, int dc, void* ptr) {
  g_dc = dc;
  rtcSetOpenCallback(dc, onDcOpen);
  rtcSetClosedCallback(dc, onDcClosed);
  rtcSetErrorCallback(dc, onDcError);
  rtcSetMessageCallback(dc, onDcMessage);
  pushEvt(6, pc, dc, "dc", "");
}

// ---------- exported API (__cdecl, for GameMaker dll_cdecl) ----------
extern "C" {

__declspec(dllexport) double __cdecl net_version(void) {
  return 2.0;
}

__declspec(dllexport) double __cdecl net_init(void) {
  rtcInitLogger(RTC_LOG_WARNING, NULL);
  rtcPreload();
  return 1.0;
}

__declspec(dllexport) void __cdecl net_terminate(void) {
  if (g_pc >= 0) {
    rtcClosePeerConnection(g_pc);
    rtcDeletePeerConnection(g_pc);
    g_pc = -1;
  }
  g_dc = -1;
  rtcCleanup();
}

__declspec(dllexport) double __cdecl net_add_ice_server(const char* uri) {
  if (!uri || !*uri) return 0.0;
  g_ice_uris.emplace_back(uri);
  return 1.0;
}

__declspec(dllexport) double __cdecl net_create_pc(void) {
  rtcConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  std::vector<const char*> servers;
  for (auto& u : g_ice_uris) servers.push_back(u.c_str());
  if (servers.empty()) {
    static const char* def = "stun:stun.l.google.com:19302";
    servers.push_back(def);
  }
  cfg.iceServers = servers.data();
  cfg.iceServersCount = (int)servers.size();
  cfg.enableIceTcp = false;
  cfg.disableAutoNegotiation = true; // GML drives offer/answer via net_create_offer/answer
  int pc = rtcCreatePeerConnection(&cfg);
  if (pc < 0) return 0.0;
  g_pc = pc;
  rtcSetLocalDescriptionCallback(pc, onLocalDesc);
  rtcSetLocalCandidateCallback(pc, onLocalCand);
  rtcSetStateChangeCallback(pc, onStateChange);
  rtcSetDataChannelCallback(pc, onDataChannel);
  return 1.0;
}

__declspec(dllexport) void __cdecl net_close(void) {
  if (g_pc >= 0) {
    rtcClosePeerConnection(g_pc);
    rtcDeletePeerConnection(g_pc);
    g_pc = -1;
  }
  g_dc = -1;
}

__declspec(dllexport) double __cdecl net_create_offer(void) {
  if (g_pc < 0) return 0.0;
  return rtcSetLocalDescription(g_pc, "offer") == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_create_answer(void) {
  if (g_pc < 0) return 0.0;
  return rtcSetLocalDescription(g_pc, NULL) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_set_remote(const char* sdp, const char* type) {
  if (g_pc < 0 || !sdp) return 0.0;
  const char* t = (type && *type) ? type : NULL;
  return rtcSetRemoteDescription(g_pc, sdp, t) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_set_local(const char* sdp, const char* type) {
  // type is the negotiation type ("offer"/"answer", or empty for auto answer);
  // sdp is ignored because local descriptions are generated by the library.
  if (g_pc < 0) return 0.0;
  const char* t = (type && *type) ? type : NULL;
  return rtcSetLocalDescription(g_pc, t) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_add_ice(const char* mid, double index, const char* cand) {
  (void)index; // libdatachannel derives the m-line from the candidate itself
  if (g_pc < 0 || !cand) return 0.0;
  const char* m = (mid && *mid) ? mid : NULL;
  return rtcAddRemoteCandidate(g_pc, cand, m) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_create_dc(const char* label) {
  if (g_pc < 0) return 0.0;
  int dc = rtcCreateDataChannel(g_pc, label ? label : "data");
  if (dc < 0) return 0.0;
  g_dc = dc;
  rtcSetOpenCallback(dc, onDcOpen);
  rtcSetClosedCallback(dc, onDcClosed);
  rtcSetErrorCallback(dc, onDcError);
  rtcSetMessageCallback(dc, onDcMessage);
  return (double)dc;
}

__declspec(dllexport) double __cdecl net_send(const char* str) {
  if (g_dc < 0 || !str) return 0.0;
  return rtcSendMessage(g_dc, str, (int)strlen(str)) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_send_to(double id, const char* str) {
  if (id < 0 || !str) return 0.0;
  return rtcSendMessage((int)id, str, (int)strlen(str)) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_send_buf(int64_t addr, double size) {
  if (g_dc < 0 || !addr || size <= 0) return 0.0;
  return rtcSendMessage(g_dc, (const char*)addr, (int)size) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_send_buf_to(double id, int64_t addr, double size) {
  if (id < 0 || !addr || size <= 0) return 0.0;
  return rtcSendMessage((int)id, (const char*)addr, (int)size) == RTC_ERR_SUCCESS ? 1.0 : 0.0;
}

__declspec(dllexport) double __cdecl net_voice(double on) {
  (void)on;
  return 0.0; // no audio in this build
}

__declspec(dllexport) double __cdecl net_state(void) {
  if (g_pc < 0) return -1.0;
  return (double)g_state;
}

__declspec(dllexport) double __cdecl net_poll(void) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_events.empty()) {
    g_cur_valid = false;
    return -1.0;
  }
  g_cur = std::move(g_events.front());
  g_events.pop_front();
  g_cur_valid = true;
  return (double)g_cur.type;
}

__declspec(dllexport) void __cdecl net_pop(void) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (!g_events.empty()) g_events.pop_front();
  g_cur_valid = false;
}

__declspec(dllexport) double __cdecl net_event_id(void) {
  return g_cur_valid ? (double)g_cur.id : -1.0;
}

__declspec(dllexport) const char* __cdecl net_event_string(void) {
  if (!g_cur_valid) return "";
  std::strncpy(g_strbuf, g_cur.s1.c_str(), sizeof(g_strbuf) - 1);
  g_strbuf[sizeof(g_strbuf) - 1] = 0;
  return g_strbuf;
}

__declspec(dllexport) const char* __cdecl net_event_string2(void) {
  if (!g_cur_valid) return "";
  std::strncpy(g_strbuf2, g_cur.s2.c_str(), sizeof(g_strbuf2) - 1);
  g_strbuf2[sizeof(g_strbuf2) - 1] = 0;
  return g_strbuf2;
}

__declspec(dllexport) double __cdecl net_event_int(void) {
  return g_cur_valid ? (double)g_cur.n : 0.0;
}

__declspec(dllexport) double __cdecl net_event_data(int64_t addr, double maxsize) {
  if (!g_cur_valid || !addr || maxsize <= 0) return 0.0;
  size_t n = g_cur.data.size();
  if (n > (size_t)maxsize) n = (size_t)maxsize;
  memcpy((void*)addr, g_cur.data.data(), n);
  return (double)n;
}

} // extern "C"
