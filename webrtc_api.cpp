// webrtc_api.cpp - WebRTC (libdatachannel) wrapper DLL for GameMaker.
// Built with winlibs g++; links libdatachannel-static + juice + usrsctp + OpenSSL
// statically + libopus (static). Exports extern "C" __cdecl functions for
// GameMaker external_define(dll_cdecl). Callbacks from libdatachannel threads push
// events; GML polls net_poll() each step.
//
// Multi-peer: net_create_pc() returns a unique pc id (>=1). Every pc-scoped
// function takes that id as its first (double) argument. You can create many
// peer connections at once (hub/mesh). Data channels have globally-unique ids,
// so net_send_to(id, ...) works across any peer.
//
// Audio: net_voice(pc, 1) adds an Opus SendRecv track to that pc, opens the
// default WASAPI mic (48 kHz mono -> Opus 20 ms frames -> RTP/SRTP via
// libdatachannel) and plays incoming audio on the default WASAPI speaker.
// One mic feeds every pc that has voice enabled. Incoming audio from every
// peer is decoded per-track and mixed sample-accurately before playback.
// RTP timestamps are a fixed base + 960 * frameIndex (48 kHz clock) so the
// remote can reconstruct playout timing.
//
// Events (net_poll returns type, -1 if none):
//   1  local SDP ready            (s1=sdp, s2=type, pc=peer id)
//   2  local ICE candidate        (s1=cand, s2=mid, pc=peer id)
//   3  data channel open          (s1="open", n=dc id)
//   4  text message               (s1=text, n=size, pc=peer id)
//   6  remote data channel        (n=dc id, pc=peer id)
//   7  data channel closed        (n=dc id)
//   8  pc connection state change (n=rtcState, pc=peer id)
//   9  error                      (s1=error)
//   10 binary message             (n=size, data via net_event_data, pc=peer id)
//   11 audio track open           (n=audio tr id, pc=peer id)
//   12 audio track closed         (n=audio tr id, pc=peer id)
//   13 video track open           (n=video tr id, pc=peer id)
//   14 video track closed         (n=video tr id, pc=peer id)
//   15 keyframe requested         (pc=peer that must send an IDR)
//
// Video: net_video_enable() adds an H.264 SendRecv track (payload type 102,
// constrained baseline) with an H264RtpPacketizer -> RtcpSrReporter ->
// RtcpNackResponder -> PliHandler -> H264RtpDepacketizer chain. The SSRC and
// payload type are identical in the SDP and the packetizer: if they ever
// diverge, the remote depacketizer silently discards every packet. The host
// supplies raw frames (net_video_send) and chooses where decoded frames go
// (net_video_set_cb); the DLL never touches a capture device or a window.
//
// Lock order: g_mtx must never be taken while g_audio_mtx or g_video_mtx is
// held. The reverse (audio/video lock, then g_mtx) is fine.

#define RTC_STATIC
#include "mf_video.h"
#include <rtc/rtc.hpp>
#include <opus/opus.h>
#include <wels/codec_api.h>

#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------- constants ----------
const int kPcId = 0;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameSamples = 960; // 20 ms at 48 kHz
constexpr uint8_t kOpusPayloadType = 111;

// mingw headers only declare (never define) the KSDATAFORMAT_SUBTYPE_* GUIDs,
// so provide the well-known values locally.
const GUID kSubtypePCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeIEEEFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// ---------- video constants ----------
constexpr uint8_t kVideoPT = 102;            // dynamic RTP payload type
constexpr uint32_t kVideoClock = 90000;      // 90 kHz, rtc::VideoClockRate
const char *kH264Profile = "42e01f";         // constrained baseline, level 3.1
constexpr int kSelftestW = 320;
constexpr int kSelftestH = 240;
constexpr int kSelftestFrames = 90;


// ---------- event queue ----------
struct NetEvent {
  int type;
  int id;
  int n;
  int pc;
  std::string s1;
  std::string s2;
  std::vector<char> data;
};
std::mutex g_mtx; // guards queue + pc/dc/tr registry
std::deque<NetEvent> g_events;

NetEvent g_cur;
bool g_cur_valid = false;
char g_strbuf[65536];
char g_strbuf2[4096];

void pushEvt(int type, int id, int n, std::string s1, std::string s2, int pc = kPcId) {
  std::lock_guard<std::mutex> lock(g_mtx);
  g_events.push_back(NetEvent{type, id, n, pc, std::move(s1), std::move(s2), {}});
}

void pushMsg(int id, int pc, const char* data, int size) {
  std::lock_guard<std::mutex> lock(g_mtx);
  NetEvent e;
  e.type = 4;
  e.id = id;
  e.n = size;
  e.pc = pc;
  e.s1 = std::string(data, size);
  g_events.push_back(std::move(e));
}

void pushBin(int id, int pc, const char* data, int size) {
  std::lock_guard<std::mutex> lock(g_mtx);
  NetEvent e;
  e.type = 10;
  e.id = id;
  e.n = size;
  e.pc = pc;
  e.data.assign(data, data + size);
  g_events.push_back(std::move(e));
}

// ---------- pc / dc / track registry ----------
std::map<int, std::shared_ptr<rtc::PeerConnection>> g_pcs;
std::map<int, std::shared_ptr<rtc::DataChannel>> g_dcs;
std::map<int, std::shared_ptr<rtc::Track>> g_trs;
std::map<int, int> g_states; // pc id -> last rtcState
std::atomic<int> g_last_pc{-1};
std::atomic<int> g_dc_last{-1};
std::atomic<int> g_tr_last{-1};
std::atomic<int> g_next_id{1};
std::atomic<int> g_next_pc{1};
std::vector<std::string> g_ice_uris;

int newId() { return g_next_id.fetch_add(1); }
int newPcId() { return g_next_pc.fetch_add(1); }

std::shared_ptr<rtc::PeerConnection> findPc(int pc) {
  auto it = g_pcs.find(pc);
  return it == g_pcs.end() ? nullptr : it->second;
}

// ---------- audio engine ----------
std::atomic<bool> g_audio_running(false);
std::atomic<bool> g_mic_mute(false);
std::atomic<float> g_volume(1.0f);
std::atomic<int> g_engine_users(0);
std::atomic<uint32_t> g_rtp_ts(48000);

std::mutex g_audio_mtx; // guards g_audio_trs, g_enc, g_decs, g_playout
std::map<int, std::shared_ptr<rtc::Track>> g_audio_trs;   // pc id -> local send track
OpusEncoder* g_enc = nullptr;
std::map<int, OpusDecoder*> g_decs;                       // track id -> decoder
std::map<int, std::deque<std::vector<int16_t>>> g_playout; // track id -> 20ms decoded chunks

// diagnostics (return values only in debug builds at runtime)
std::atomic<int> g_diag_encoded(0);
std::atomic<int> g_diag_sent(0);
std::atomic<int> g_diag_decoded(0);
std::atomic<int> g_diag_playout(0);
std::atomic<int> g_diag_pb_loops(0);   // playback loop iterations (heartbeat)
std::atomic<int> g_diag_pb_write(0);   // frames handed to WASAPI render
std::atomic<int> g_diag_pb_none(0);    // write iterations where the queue was empty
std::atomic<int> g_diag_pb_pop(0);     // chunks actually consumed from the queue
std::atomic<int> g_diag_onframe(0);    // onAudioFrame invocations
std::atomic<int> g_diag_nodec(0);      // onAudioFrame with no decoder found
std::atomic<int> g_diag_opusbad(0);    // opus_decode returned <= 0
std::atomic<int> g_diag_trid(0);       // last trId seen in onAudioFrame
std::atomic<int> g_diag_decs_size(0);  // number of registered decoders

std::thread g_capture_thread;
std::thread g_playback_thread;

// ---------- video state ----------
std::mutex g_video_mtx;                                  // g_vtrs, g_vdecs, g_vstat
std::map<int, std::shared_ptr<rtc::Track>> g_vtrs;       // pc id -> local video track
// A per-track decoder plus the lock that keeps it alive for as long as
// anything might be using it. onVideoFrame runs on the transport thread while
// the track can be closed (and its decoder destroyed) on another, so decode
// and destruction have to be mutually exclusive - otherwise OpenH264 is torn
// down underneath an in-flight decode.
struct VDec {
  ISVCDecoder *dec = nullptr;
  std::mutex mtx;
  bool closed = false;
};
std::map<int, std::shared_ptr<VDec>> g_vdecs;          // track id -> decoder
struct VStat {
  int frames = 0;     // decoded pictures
  int decErr = 0;     // decoder failures
  int w = 0, h = 0;   // dimensions of the last decoded picture
  uint32_t luma = 0;  // checksum of the last luma plane (0 == blank)
  // Per-frame mean luma, capped. The self-test compares the stream that
  // arrived against a local decode of the same bitstream; if a frame was
  // mangled in transit its mean moves and the mismatch is caught.
  std::deque<float> means;
};
std::map<int, VStat> g_vstat;                            // track id -> stats
std::map<int, int> g_trpc;                                            // track id -> pc id
std::map<int, std::string> g_vmid;                       // pc id -> video m-line mid

std::atomic<int> g_diag_v_encoded(0);
std::atomic<int> g_diag_v_sent(0);
std::atomic<int> g_diag_v_idr(0);
std::atomic<int> g_diag_v_rtp(0);
std::atomic<int> g_diag_v_frames(0);
std::atomic<int> g_diag_v_decerr(0);
std::atomic<int> g_diag_v_pli(0);
std::atomic<int> g_diag_v_tracks(0);
std::atomic<bool> g_v_need_idr{false};                  // set by PliHandler
// self-test verification results
std::atomic<int> g_diag_v_compared(0);                  // frames compared
std::atomic<int> g_diag_v_mismatch(0);                  // frames that did not match
std::atomic<int> g_diag_v_baddim(0);                    // frames with wrong size
std::atomic<double> g_diag_v_dev(0.0);                  // worst mean-luma deviation
std::atomic<double> g_diag_v_encms(0.0);                // worst single-frame encode ms
std::atomic<int> g_diag_hwpath(0);                      // 1 when a GPU encoder is in use
std::atomic<double> g_diag_v_kb(0.0);                   // total encoded payload (KB)

// Lossless-pipeline instrumentation. dropIn and dropOut are the ones that must
// stay at zero: a discarded host frame or a discarded encoded access unit both
// orphan an H.264 reference chain, which the receiver cannot detect without
// frame identity. Everything else is measurement, not policy.
std::atomic<uint64_t> g_diag_v_dropIn(0);    // host frames discarded
std::atomic<uint64_t> g_diag_v_dropOut(0);   // encoded access units discarded
std::atomic<uint64_t> g_diag_v_blocked(0);   // times backpressure engaged
std::atomic<uint64_t> g_diag_v_harnessLate(0); // harness missed its own schedule
std::atomic<uint64_t> g_diag_v_seqTx(0);     // highest sequence submitted
std::atomic<uint64_t> g_diag_v_seqRx(0);     // highest sequence presented
std::atomic<uint64_t> g_diag_v_repeats(0);   // filler submissions to the MFT
std::atomic<uint64_t> g_diag_v_submitted(0); // inputs the MFT accepted
std::atomic<uint64_t> g_diag_v_queued(0);     // frames not yet encoded

// create/destroy helpers for the per-track decoder
std::shared_ptr<VDec> makeDecoderEntry() {
  ISVCDecoder *dec = nullptr;
  if (WelsCreateDecoder(&dec) != 0 || !dec) return nullptr;
  SDecodingParam dp;
  std::memset(&dp, 0, sizeof(dp));
  dp.eEcActiveIdc = ERROR_CON_DISABLE; // keep output pristine for verification
  dp.sVideoProperty.size = sizeof(SVideoProperty);
  if (dec->Initialize(&dp) != 0) {
    WelsDestroyDecoder(dec);
    return nullptr;
  }
  auto v = std::make_shared<VDec>();
  v->dec = dec;
  return v;
}

void dropDecoder(int trId) {
  std::shared_ptr<VDec> v;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vdecs.find(trId);
    if (it != g_vdecs.end()) {
      v = it->second;
      g_vdecs.erase(it);
    }
    g_vstat.erase(trId);
    g_trpc.erase(trId);
  }
  if (!v) return;
  // Take the decode lock before destroying, so a decode already in flight on
  // the transport thread finishes first.
  std::lock_guard<std::mutex> lk(v->mtx);
  if (v->closed) return;
  v->closed = true;
  if (v->dec) {
    v->dec->Uninitialize();
    WelsDestroyDecoder(v->dec);
    v->dec = nullptr;
  }
}

// ---------- video send + delivery state ----------
// Pixel formats accepted by net_video_send and reported to the frame callback.
enum { kFmtI420 = 0, kFmtNV12 = 1, kFmtBGRA = 2 };

// Signature the host registers to receive decoded frames. Invoked on a
// dedicated DLL thread, never on a libdatachannel thread.
typedef void(__cdecl *net_video_cb)(int pc, int w, int h, int stride, int format,
                                    const void *data, double tsMs, double user);

// Encoder + pacing for one peer connection.
struct VSender {
  ISVCEncoder *enc = nullptr;
  // Hardware encoder, preferred. Media Foundation fronts NVENC, Quick Sync and
  // VCN behind one interface, so this covers NVIDIA, Intel and AMD, discrete
  // and integrated, without a vendor SDK. It is an optimisation and never a
  // requirement: when it is null, or when it produces nothing, everything
  // falls back to the software encoder below.
  std::unique_ptr<MfEncoder> hw;
  bool hwTried = false;
  std::mutex mtx;            // guards everything below and the encoder
  uint32_t ts = 0;
  int w = 0, h = 0, fps = 30, kbps = 2500, gopSec = 1;
  bool wantIdr = true;       // the first frame after opening must be an IDR
  uint64_t encNs = 0;        // total encode time
  int encoded = 0, sent = 0, dropped = 0;
};
std::map<int, std::shared_ptr<VSender>> g_vsenders; // pc id -> sender

// One decoded-frame queue per peer connection. Bounded, and full means the
// producer waits rather than overwrites. It used to be a single slot that kept
// "latency flat" by discarding frames the host had not delivered yet - which is
// not flat latency, it is invisible loss, and the host had no way to tell.
struct VOut {
  // A decoded frame, delivered in full or not at all. A single reusable buffer
  // cannot express that: the moment two frames are in flight, one of them has
  // to be overwritten, and the host sees a torn or stale picture with nothing
  // to tell it which.
  struct Frame {
    std::vector<uint8_t> data;
    int w = 0, h = 0, stride = 0, cstride = 0, fmt = kFmtI420;
    double ts = 0;
  };
  std::mutex mtx;                 // guards cb/user and the drain handshake
  std::condition_variable cv;
  LosslessQ<Frame> q{8};
  net_video_cb cb = nullptr;
  double user = 0;
  std::thread th;
  std::atomic<bool> run{false};
  std::atomic<int> delivered{0};
  std::atomic<int> skipped{0};    // retired: kept only so the diag slot stays valid
};
std::map<int, std::shared_ptr<VOut>> g_vouts; // pc id -> output mailbox

// ---------- colour conversion ----------
// Interleaved BGRA (what GDI/D3D hand back) -> I420 planar, BT.601 *limited*
// range. The forward and inverse below are exact integer inverses of each
// other, which matters: get the chroma centring wrong and you get magenta.
static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void bgraToI420(const uint8_t *srcIn, int w, int h, int stride, std::vector<uint8_t> &yOut,
                std::vector<uint8_t> &uOut, std::vector<uint8_t> &vOut) {
  const int cw = w / 2, ch = h / 2;
  yOut.resize((size_t)w * h);
  uOut.resize((size_t)cw * ch);
  vOut.resize((size_t)cw * ch);
  const uint8_t *src = srcIn;
  uint8_t *yp = yOut.data();
  uint8_t *up = uOut.data();
  uint8_t *vp = vOut.data();
  // luma
  for (int j = 0; j < h; j++) {
    const uint8_t *row = src + (size_t)j * stride;
    for (int i = 0; i < w; i++) {
      const int b = row[i * 4 + 0], g = row[i * 4 + 1], r = row[i * 4 + 2];
      *yp++ = clamp255((((66 * r + 129 * g + 25 * b + 128) >> 8) + 16));
    }
  }
  // chroma from the averaged 2x2 block
  for (int j = 0; j < ch; j++) {
    for (int i = 0; i < cw; i++) {
      int b = 0, g = 0, r = 0;
      for (int dy = 0; dy < 2; dy++) {
        const uint8_t *row = src + (size_t)(j * 2 + dy) * stride + (size_t)(i * 2) * 4;
        for (int dx = 0; dx < 2; dx++) {
          b += row[dx * 4 + 0];
          g += row[dx * 4 + 1];
          r += row[dx * 4 + 2];
        }
      }
      b >>= 2; g >>= 2; r >>= 2;
      *up++ = clamp255(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      *vp++ = clamp255(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
}

// I420 planar -> BGRA, for hosts that want to blit straight to a window.
// Exact inverse of the chroma step above.
void i420ToBgra(const uint8_t *y, const uint8_t *u, const uint8_t *v, int w, int h, int yStride,
                int cStride, std::vector<uint8_t> &out) {
  out.resize((size_t)w * h * 4);
  for (int j = 0; j < h; j++) {
    const uint8_t *yr = y + (size_t)j * yStride;
    const uint8_t *ur = u + (size_t)(j / 2) * cStride;
    const uint8_t *vr = v + (size_t)(j / 2) * cStride;
    uint8_t *d = out.data() + (size_t)j * w * 4;
    for (int i = 0; i < w; i++) {
      const int Y = yr[i] - 16, U = ur[i / 2] - 128, V = vr[i / 2] - 128;
      d[i * 4 + 0] = clamp255((298 * Y + 516 * U + 128) >> 8);           // B
      d[i * 4 + 1] = clamp255((298 * Y - 100 * U - 208 * V + 128) >> 8); // G
      d[i * 4 + 2] = clamp255((298 * Y + 409 * V + 128) >> 8);           // R
      d[i * 4 + 3] = 0xFF;
    }
  }
}

// Pack a decoded picture into the host mailbox for its peer connection. Only
// the newest frame is kept: if the host is slower than the frame rate, the
// previous frame is discarded rather than queued, which is what stops latency
// from growing without bound.
void handFrameToHost(int trId, int w, int h, int yStride, int cStrideIn, uint8_t *const dst[4]) {
  int pc = -1;
  std::shared_ptr<VOut> o;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto t = g_trpc.find(trId);
    if (t == g_trpc.end()) return;
    pc = t->second;
    auto it = g_vouts.find(pc);
    if (it == g_vouts.end()) return;
    o = it->second;
    if (!o->cb) return; // nobody listening: do not copy
  }
  const int cStride = cStrideIn > 0 ? cStrideIn : w / 2;
  const size_t ySize = (size_t)yStride * h;
  const size_t cSize = (size_t)cStride * (h / 2);
  // Blocks when the host has fallen 8 frames behind, rather than overwriting the
  // frame it has not delivered yet.
  VOut::Frame f;
  f.data.assign(dst[0], dst[0] + ySize);
  f.data.insert(f.data.end(), dst[1], dst[1] + cSize);
  f.data.insert(f.data.end(), dst[2], dst[2] + cSize);
  f.w = w;
  f.h = h;
  f.stride = yStride;
  f.cstride = cStride;
  f.fmt = kFmtI420;
  f.ts = (double)GetTickCount64();
  o->q.push(std::move(f));
}


void videoOutThread(int pc, std::shared_ptr<VOut> o) {
  std::vector<uint8_t> bgra;
  while (o->run.load()) {
    VOut::Frame f;
    if (!o->q.pop(f, 200)) continue;   // times out, or closed at shutdown
    net_video_cb cb = nullptr;
    double user = 0;
    {
      std::lock_guard<std::mutex> lk(o->mtx);
      cb = o->cb;
      user = o->user;
    }
    if (!cb) continue;
    const int w = f.w, h = f.h, stride = f.stride, cstride = f.cstride, fmt = f.fmt;
    const double ts = f.ts;
    std::vector<uint8_t> &local = f.data;

    if (fmt == kFmtBGRA) {
      // hand the host exactly what it asked for
      try { cb(pc, w, h, stride, kFmtBGRA, local.data(), ts, user); } catch (...) {}
    } else {
      const size_t ySize = (size_t)stride * h;
      const size_t cSize = (size_t)cstride * (h / 2);
      i420ToBgra(local.data(), local.data() + ySize, local.data() + ySize + cSize,
                 w, h, stride, cstride, bgra);
      try { cb(pc, w, h, w * 4, kFmtBGRA, bgra.data(), ts, user); } catch (...) {}
    }
    o->delivered.fetch_add(1);
    g_diag_v_seqRx.fetch_add(1);
  }
}

// or video, so the video m-line is identified by the mid we negotiated for it.
// Extracts the a=mid: of the m=video section from a local SDP.
bool parseVideoMid(const std::string &sdp, std::string &midOut) {
  size_t m = sdp.find("m=video");
  if (m == std::string::npos) return false;
  size_t sectionEnd = sdp.find("\nm=", m + 1);
  size_t a = sdp.find("a=mid:", m);
  if (a == std::string::npos) return false;
  if (sectionEnd != std::string::npos && a > sectionEnd) return false;
  size_t b = a + 6;
  while (b < sdp.size() && (sdp[b] == ' ' || sdp[b] == '\t')) b++;
  size_t e = sdp.find_first_of("\r\n", b);
  if (e == std::string::npos) e = sdp.size();
  midOut = sdp.substr(b, e - b);
  return !midOut.empty();
}


// ---------- sample conversion ----------
struct DevFmt {
  bool valid = false;
  bool isFloat = false;
  int rate = 0;
  int channels = 0;
  int bits = 0;
};

void probeFormat(const WAVEFORMATEX* wf, DevFmt& f) {
  f = DevFmt{};
  if (!wf) return;
  f.rate = wf->nSamplesPerSec;
  f.channels = wf->nChannels;
  f.bits = wf->wBitsPerSample;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const WAVEFORMATEXTENSIBLE* we = (const WAVEFORMATEXTENSIBLE*)wf;
    f.bits = we->Samples.wValidBitsPerSample ? (int)we->Samples.wValidBitsPerSample : wf->wBitsPerSample;
    if (IsEqualGUID(we->SubFormat, kSubtypeIEEEFloat))
      f.isFloat = true;
    else if (IsEqualGUID(we->SubFormat, kSubtypePCM))
      f.isFloat = false;
    else
      return; // unsupported subtype
  } else if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    f.isFloat = true;
  } else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
    return; // unsupported
  }
  if (f.channels <= 0 || f.rate <= 0) return;
  if (!f.isFloat && f.bits != 8 && f.bits != 16 && f.bits != 24 && f.bits != 32) return;
  if (f.isFloat && f.bits != 32) return;
  f.valid = true;
}

// interleaved device samples -> mono float at the device rate
void toMonoFloat(const BYTE* src, UINT32 frames, const DevFmt& f, std::vector<float>& out) {
  out.resize(frames);
  int ch = f.channels;
  if (f.isFloat) {
    const float* in = (const float*)src;
    for (UINT32 i = 0; i < frames; i++) {
      float s = 0;
      for (int c = 0; c < ch; c++) s += in[(size_t)i * ch + c];
      out[i] = s / ch;
    }
    return;
  }
  int bytesPer = f.bits / 8;
  for (UINT32 i = 0; i < frames; i++) {
    double s = 0;
    for (int c = 0; c < ch; c++) {
      const BYTE* p = src + ((size_t)i * ch + c) * bytesPer;
      double v = 0;
      switch (f.bits) {
        case 8:  v = ((int)p[0] - 128) / 128.0; break;
        case 16: { int16_t x; memcpy(&x, p, 2); v = x / 32768.0; break; }
        case 24: { int32_t x = ((int)p[0] << 8) | ((int)p[1] << 16) | ((int)p[2] << 24); x >>= 8; v = x / 8388608.0; break; }
        case 32: { int32_t x; memcpy(&x, p, 4); v = x / 2147483648.0; break; }
      }
      s += v;
    }
    out[i] = (float)(s / ch);
  }
}

// mono float -> interleaved device samples
void fromMonoFloat(const std::vector<float>& mono, const DevFmt& f, std::vector<BYTE>& out) {
  int ch = f.channels;
  int bytesPer = f.bits / 8;
  out.resize(mono.size() * ch * bytesPer);
  BYTE* dst = out.data();
  for (size_t i = 0; i < mono.size(); i++) {
    float v = mono[i];
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    for (int c = 0; c < ch; c++) {
      if (f.isFloat) {
        memcpy(dst, &v, 4);
        dst += 4;
      } else if (f.bits == 8) {
        *dst++ = (BYTE)(int)(v * 127.0f + 128.0f);
      } else if (f.bits == 16) {
        int16_t x = (int16_t)(v * 32767.0f);
        memcpy(dst, &x, 2);
        dst += 2;
      } else if (f.bits == 24) {
        int32_t x = (int32_t)(v * 8388607.0f);
        dst[0] = (BYTE)(x & 0xFF);
        dst[1] = (BYTE)((x >> 8) & 0xFF);
        dst[2] = (BYTE)((x >> 16) & 0xFF);
        dst += 3;
      } else {
        int32_t x = (int32_t)(v * 2147483647.0f);
        memcpy(dst, &x, 4);
        dst += 4;
      }
    }
  }
}

// linear resampler for mono float, keeps fractional position across calls
struct MonoResampler {
  double pos = 0.0; // next output sample's input position, relative to current buffer

  void process(const float* in, size_t n, int inRate, int outRate, std::vector<float>& out) {
    out.clear();
    if (n == 0) return;
    if (inRate == outRate) {
      out.assign(in, in + n);
      return;
    }
    double step = (double)inRate / (double)outRate;
    while (pos < (double)n) {
      int i0 = (int)pos;
      if (i0 >= (int)n) break;
      int i1 = i0 + 1;
      if (i1 >= (int)n) i1 = (int)n - 1;
      double frac = pos - i0;
      out.push_back((float)(in[i0] + (in[i1] - in[i0]) * frac));
      pos += step;
    }
    pos -= n;
  }
};

} // namespace

// ---------- forward declarations ----------
namespace {
void onAudioFrame(int trId, const rtc::binary& data, const rtc::FrameInfo& info);
void sendOpusFrame(const unsigned char* pkt, int n);
void sendOpusFrameFromCapture(const opus_int16* pcm);
void stopAllAudio();
void registerDataChannel(std::shared_ptr<rtc::DataChannel> dc, int pc);
void createAudioTrack(int pc);
void handleRemoteTrack(int pc, std::shared_ptr<rtc::Track> tr);
void createVideoTrack(int pc);
void onVideoFrame(int trId, const rtc::binary &data);
void videoTrackClosed(int pc, int trId);
void handleRemoteVideoTrack(int pc, std::shared_ptr<rtc::Track> tr);
}

// ---------- audio receive / send ----------
namespace {

void onAudioFrame(int trId, const rtc::binary& data, const rtc::FrameInfo& info) {
  (void)info;
  g_diag_onframe.fetch_add(1);
  g_diag_trid.store(trId);
  if (!g_audio_running.load()) return;
  OpusDecoder* dec = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    auto it = g_decs.find(trId);
    if (it != g_decs.end()) dec = it->second;
    g_diag_decs_size.store((int)g_decs.size());
  }
  if (!dec) {
    g_diag_nodec.fetch_add(1);
    return;
  }
  opus_int16 pcm[kFrameSamples];
  int n = opus_decode(dec, (const unsigned char*)data.data(), (int)data.size(), pcm, kFrameSamples, 0);
  if (n <= 0) {
    g_diag_opusbad.fetch_add(1);
    return;
  }
  g_diag_decoded.fetch_add(1);
  float vol = g_volume.load();
  std::vector<int16_t> chunk((size_t)n);
  for (int i = 0; i < n; i++) {
    long v = (long)((float)pcm[i] * vol);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    chunk[i] = (int16_t)v;
  }
  std::lock_guard<std::mutex> lk(g_audio_mtx);
  auto& q = g_playout[trId]; // separate mixer lane per incoming track
  if (q.size() > 500) q.pop_front(); // drop oldest if >10 s behind
  q.push_back(std::move(chunk));
}

void sendOpusFrame(const unsigned char* pkt, int n) {
  // One mic feed: broadcast the same frame to every voice-enabled pc.
  std::vector<std::shared_ptr<rtc::Track>> tracks;
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    for (auto& kv : g_audio_trs) {
      if (kv.second && kv.second->isOpen()) tracks.push_back(kv.second);
    }
  }
  if (tracks.empty()) return;
  g_diag_sent.fetch_add((int)tracks.size());
  uint32_t ts = g_rtp_ts.fetch_add((uint32_t)kFrameSamples);
  rtc::binary payload(reinterpret_cast<const rtc::byte*>(pkt),
                      reinterpret_cast<const rtc::byte*>(pkt) + n);
  rtc::FrameInfo fi(ts);
  for (auto& tr : tracks) {
    try {
      rtc::binary copy = payload;
      tr->sendFrame(std::move(copy), fi);
    } catch (...) {
    }
  }
}

void sendOpusFrameFromCapture(const opus_int16* pcm) {
  OpusEncoder* enc = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    enc = g_enc;
  }
  if (!enc) return;
  unsigned char packet[1500];
  int n = opus_encode(enc, pcm, kFrameSamples, packet, sizeof(packet));
  if (n > 0) {
    g_diag_encoded.fetch_add(1);
    sendOpusFrame(packet, n);
  }
}

// ---------- capture thread ----------
void captureThreadMain() {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool coOk = (co == S_OK || co == S_FALSE);

  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* mix = nullptr;
  HANDLE hEvt = INVALID_HANDLE_VALUE;
  MonoResampler resampler;
  std::deque<int16_t> ring;
  std::vector<float> mono;
  std::vector<float> resampled;
  DevFmt f;
  bool ok = false;

  auto cleanup = [&]() {
    if (client) {
      if (ok) client->Stop();
      client->Release();
    }
    if (capture) capture->Release();
    if (mix) CoTaskMemFree(mix);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (hEvt != INVALID_HANDLE_VALUE) CloseHandle(hEvt);
  };

  if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&enumerator))) &&
      SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device)) &&
      SUCCEEDED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&client)) &&
      SUCCEEDED(client->GetMixFormat(&mix))) {
    probeFormat(mix, f);
    if (f.valid &&
        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     0, 0, mix, nullptr))) {
      hEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (hEvt && SUCCEEDED(client->SetEventHandle(hEvt)) &&
          SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture)))) {
        if (SUCCEEDED(client->Start())) ok = true;
      }
    }
  }

  if (!ok) pushEvt(9, kPcId, 0, "audio capture unavailable", "");

  while (g_audio_running.load() && ok) {
    WaitForSingleObject(hEvt, 100);
    UINT32 packetLen = 0;
    while (capture->GetNextPacketSize(&packetLen) == S_OK && packetLen > 0) {
      BYTE* data = nullptr;
      UINT32 numFrames = 0;
      DWORD flags = 0;
      if (SUCCEEDED(capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
        toMonoFloat(data, numFrames, f, mono);
        resampler.process(mono.data(), mono.size(), f.rate, kSampleRate, resampled);
        for (size_t i = 0; i < resampled.size(); i++) {
          float v = resampled[i];
          if (v < -1.0f) v = -1.0f;
          if (v > 1.0f) v = 1.0f;
          ring.push_back((int16_t)(v * 32767.0f));
        }
        capture->ReleaseBuffer(numFrames);
      }
    }
    while (ring.size() >= (size_t)kFrameSamples) {
      opus_int16 pcm[kFrameSamples];
      for (int i = 0; i < kFrameSamples; i++) {
        pcm[i] = ring.front();
        ring.pop_front();
      }
      if (g_mic_mute.load()) memset(pcm, 0, sizeof(pcm));
      sendOpusFrameFromCapture(pcm);
    }
  }

  cleanup();
  if (coOk) CoUninitialize();
}

// ---------- playback thread ----------
void playbackThreadMain() {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool coOk = (co == S_OK || co == S_FALSE);

  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  WAVEFORMATEX* mix = nullptr;
  HANDLE hEvt = INVALID_HANDLE_VALUE;
  MonoResampler resampler;
  std::vector<float> mono;
  std::vector<float> resampled;
  std::vector<BYTE> devbuf;
  std::vector<int16_t> full;
  DevFmt f;
  UINT32 bufsize = 0;
  bool ok = false;
  std::string failstage;

  auto cleanup = [&]() {
    if (client) {
      if (ok) client->Stop();
      client->Release();
    }
    if (render) render->Release();
    if (mix) CoTaskMemFree(mix);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (hEvt != INVALID_HANDLE_VALUE) CloseHandle(hEvt);
  };

  if (!SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)))) failstage = "CoCreateInstance";
  else if (!SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &device))) failstage = "GetDefaultAudioEndpoint";
  else if (!SUCCEEDED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&client))) failstage = "Activate client";
  else if (!SUCCEEDED(client->GetMixFormat(&mix))) failstage = "GetMixFormat";
  else {
    probeFormat(mix, f);
    if (!f.valid) failstage = "unsupported format";
    else if (!SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           0, 0, mix, nullptr))) failstage = "Initialize";
    else {
      hEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!hEvt) failstage = "CreateEvent";
      else if (!SUCCEEDED(client->SetEventHandle(hEvt))) failstage = "SetEventHandle";
      else if (!SUCCEEDED(client->GetBufferSize(&bufsize))) failstage = "GetBufferSize";
      else if (!SUCCEEDED(client->GetService(IID_PPV_ARGS(&render)))) failstage = "GetService render";
      else if (!SUCCEEDED(client->Start())) failstage = "Start";
      else ok = true;
    }
  }

  if (!ok) pushEvt(9, kPcId, 0, "audio playback unavailable: " + failstage, "");
  else pushEvt(9, kPcId, 0, "playback started", "");

  int bytesPer = f.bits / 8;
  std::vector<int16_t> srcbuf; // sample-accurate combined monitor buffer

  while (g_audio_running.load() && ok) {
    g_diag_pb_loops.fetch_add(1);
    WaitForSingleObject(hEvt, 100);
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) continue;
    UINT32 avail = bufsize > padding ? bufsize - padding : 0;
    while (avail > 0) {
      UINT32 n = avail > (UINT32)kFrameSamples ? (UINT32)kFrameSamples : avail;
      g_diag_pb_write.fetch_add((int)n);
      // Build one 20ms mix step whenever the combined buffer runs low.
      while (srcbuf.size() < (size_t)n) {
        std::vector<int16_t> mixed(kFrameSamples, 0);
        bool got = false;
        {
          std::lock_guard<std::mutex> lk(g_audio_mtx);
          for (auto& kv : g_playout) {
            auto& q = kv.second;
            if (q.empty()) continue;
            got = true;
            g_diag_pb_pop.fetch_add(1);
            const auto& c = q.front();
            for (size_t i = 0; i < c.size() && i < mixed.size(); i++) {
              long v = (long)mixed[i] + (long)c[i];
              if (v > 32767) v = 32767;
              if (v < -32768) v = -32768;
              mixed[i] = (int16_t)v;
            }
            q.pop_front();
          }
        }
        if (!got) g_diag_pb_none.fetch_add(1);
        srcbuf.insert(srcbuf.end(), mixed.begin(), mixed.end());
      }
      BYTE* dst = nullptr;
      if (FAILED(render->GetBuffer(n, &dst))) break;

      // exactly n frames from srcbuf (silence-pad if ever short)
      full.resize(n);
      size_t k = srcbuf.size() < (size_t)n ? srcbuf.size() : (size_t)n;
      if (k > 0) memcpy(full.data(), srcbuf.data(), k * sizeof(int16_t));
      if (k < n) memset(full.data() + k, 0, (n - k) * sizeof(int16_t));
      if (k > 0) srcbuf.erase(srcbuf.begin(), srcbuf.begin() + k);

      mono.resize(n);
      for (UINT32 i = 0; i < n; i++) mono[i] = (float)full[i] / 32768.0f;
      resampler.process(mono.data(), n, kSampleRate, f.rate, resampled);
      fromMonoFloat(resampled, f, devbuf);
      size_t want = (size_t)n * (size_t)f.channels * bytesPer;
      if (devbuf.size() > want) devbuf.resize(want);
      memcpy(dst, devbuf.data(), devbuf.size());
      if (devbuf.size() < want) memset(dst + devbuf.size(), 0, want - devbuf.size());

      render->ReleaseBuffer(n, 0);
      avail -= n;
    }
  }

  cleanup();
  if (coOk) CoUninitialize();
}

// ---------- engine lifecycle ----------
void audioRef() {
  if (g_engine_users.fetch_add(1) != 0) return;
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_playout.clear();
    int err = 0;
    g_enc = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !g_enc) {
      if (g_enc) { opus_encoder_destroy(g_enc); g_enc = nullptr; }
    } else {
      opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(32000));
    }
  }
  g_rtp_ts.store(48000);
  g_audio_running.store(true);
  g_capture_thread = std::thread(captureThreadMain);
  g_playback_thread = std::thread(playbackThreadMain);
}

void stopAllAudio() {
  if (g_engine_users.exchange(0) == 0) return;
  g_audio_running.store(false);
  if (g_capture_thread.joinable()) g_capture_thread.join();
  if (g_playback_thread.joinable()) g_playback_thread.join();
  std::lock_guard<std::mutex> lk(g_audio_mtx);
  if (g_enc) { opus_encoder_destroy(g_enc); g_enc = nullptr; }
  for (auto& kv : g_decs)
    if (kv.second) opus_decoder_destroy(kv.second);
  g_decs.clear();
  g_playout.clear();
}

void audioUnref() {
  if (g_engine_users.fetch_sub(1) != 1) return;
  stopAllAudio();
}

// ---------- track setup ----------
void createAudioTrack(int pc) {
  std::shared_ptr<rtc::PeerConnection> cpc;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    cpc = findPc(pc);
  }
  if (!cpc) return;

  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (g_audio_trs.find(pc) != g_audio_trs.end()) return; // already have one
  }

  uint32_t ssrc = 0x13000000u + (uint32_t)(GetTickCount() & 0xFFFFu) + (uint32_t)pc * 997u;

  rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
  audio.addOpusCodec(kOpusPayloadType);
  audio.addSSRC(ssrc, "voice", "stream0", std::to_string(pc).c_str());

  std::shared_ptr<rtc::Track> tr;
  try {
    tr = cpc->addTrack(std::move(audio));
  } catch (...) {
    pushEvt(9, kPcId, 0, "failed to add audio track", "");
    return;
  }

  int id = newId();
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_trs[id] = tr;
  }

  // This is a SendRecv track: the peer's audio comes back on this same
  // track's onFrame, so give the local id its own decoder + mixer lane.
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(kSampleRate, kChannels, &err);

  auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
      ssrc, "voice", kOpusPayloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
  auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
  auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
  packetizer->addToChain(srReporter);
  auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
  packetizer->addToChain(nackResponder);
  auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
  packetizer->addToChain(depacketizer);
  tr->setMediaHandler(packetizer);

  tr->onOpen([pc, id] { pushEvt(11, id, id, "audio open", "", pc); });
  tr->onClosed([pc, id] {
    {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      g_audio_trs.erase(pc);
      g_playout.erase(id);
      auto it = g_decs.find(id);
      if (it != g_decs.end()) {
        if (it->second) opus_decoder_destroy(it->second);
        g_decs.erase(it);
      }
    }
    // Do not audioUnref here — net_voice(pc,0) / net_close_pc already do it.
    pushEvt(12, id, id, "audio closed", "", pc);
  });
  tr->onFrame([id](rtc::binary data, rtc::FrameInfo info) { onAudioFrame(id, data, info); });

  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (err == OPUS_OK && dec) g_decs[id] = dec;
    else if (dec) opus_decoder_destroy(dec);
    g_audio_trs[pc] = tr;
  }
}

// A remote peer's audio track -> its own decoder + mixer lane.
void handleRemoteTrack(int pc, std::shared_ptr<rtc::Track> tr) {
  int id = newId();
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_trs[id] = tr;
  }
  // Without a depacketizer in the handler chain, incoming RTP is never
  // converted to frames and onFrame never fires.
  tr->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(kSampleRate, kChannels, &err);
  if (err == OPUS_OK && dec) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_decs[id] = dec;
  }
  audioRef(); // playback lane may be needed
  tr->onClosed([pc, id] {
    {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      auto it = g_decs.find(id);
      if (it != g_decs.end()) {
        if (it->second) opus_decoder_destroy(it->second);
        g_decs.erase(it);
      }
      g_playout.erase(id);
    }
    audioUnref();
    pushEvt(12, id, id, "audio closed", "", pc);
  });
  tr->onFrame([id](rtc::binary data, rtc::FrameInfo info) { onAudioFrame(id, data, info); });
  pushEvt(11, id, id, "audio open", "", pc);
}

// ---------- video receive ----------
// A complete access unit arrived on a remote track (the depacketizer
// reassembled it). Decode and account for it. Where the picture goes next is
// the host's decision, not ours.
// Ask the sender for a fresh keyframe. Rate limited, because a decoder that is
// still warming up can fail a few frames in a row and a PLI storm makes the
// sender's bitrate collapse.
static void requestKeyframeFromSender(int trId) {
  static std::mutex mtx;
  static std::map<int, ULONGLONG> lastAt;
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = lastAt.find(trId);
    if (it != lastAt.end() && now - it->second < 300) return;
    lastAt[trId] = now;
  }
  std::shared_ptr<rtc::Track> tr;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_trs.find(trId);
    if (it != g_trs.end()) tr = it->second;
  }
  if (tr) {
    try {
      tr->requestKeyframe();  // sends a PLI upstream
    } catch (...) {
    }
  }
}

void onVideoFrame(int trId, const rtc::binary &data) {
  g_diag_v_rtp.fetch_add(1);
  // Hold a reference so the entry cannot be destroyed underneath us, and take
  // its lock so a concurrent close waits for this decode to finish.
  std::shared_ptr<VDec> v;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vdecs.find(trId);
    if (it != g_vdecs.end()) v = it->second;
  }
  if (!v) {
    g_diag_v_decerr.fetch_add(1);
    return;
  }
  std::unique_lock<std::mutex> dlk(v->mtx);
  if (v->closed || !v->dec) {
    g_diag_v_decerr.fetch_add(1);
    return;
  }
  uint8_t *dst[4] = {nullptr, nullptr, nullptr, nullptr};
  int stride[4] = {0, 0, 0, 0};
  int w = 0, h = 0;
  DECODING_STATE st = v->dec->DecodeFrame(reinterpret_cast<const unsigned char *>(data.data()),
                                         (int)data.size(), dst, stride, w, h);
  if (st != 0 || !dst[0]) {
    g_diag_v_decerr.fetch_add(1);
    {
      std::lock_guard<std::mutex> lk(g_video_mtx);
      g_vstat[trId].decErr++;
    }
    // The reference chain is broken. Nothing downstream can recover on its own,
    // so ask for a keyframe; otherwise the picture stays frozen on the last
    // good frame until the sender's next periodic IDR.
    requestKeyframeFromSender(trId);
    return;
  }
  // checksum luma so the harness can tell "decoded" from "decoded blank",
  // and keep the per-frame mean so corruption in transit becomes detectable
  uint32_t sum = 0;
  for (int j = 0; j < h; j++) {
    const uint8_t *row = dst[0] + (size_t)j * stride[0];
    for (int i = 0; i < w; i++) sum += row[i];
  }
  g_diag_v_frames.fetch_add(1);
  {
    std::lock_guard<std::mutex> slk(g_video_mtx);
    VStat &s = g_vstat[trId];
    s.frames++;
    s.w = w;
    s.h = h;
    s.luma = sum;
    s.means.push_back(w && h ? (float)sum / (float)((size_t)w * h) : 0.0f);
    if (s.means.size() > 512) s.means.pop_front();
  }
  handFrameToHost(trId, w, h, stride[0], stride[1], dst);
}


void videoTrackClosed(int pc, int trId) {
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_vtrs.erase(pc);
  }
  dropDecoder(trId);
  pushEvt(14, trId, trId, "video closed", "", pc);
}

// A remote peer's video track -> its own decoder and depacketizer.
void handleRemoteVideoTrack(int pc, std::shared_ptr<rtc::Track> tr) {
  const int id = newId();
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_trs[id] = tr;
  }
  // Without a depacketizer, incoming RTP is never reassembled into frames and
  // onFrame never fires - which looks exactly like "the sender is broken".
  tr->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
  if (auto dec = makeDecoderEntry()) {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_vdecs[id] = dec;
    g_vstat[id] = VStat{};
  }
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_trpc[id] = pc;
  }
  tr->onClosed([pc, id] {
    dropDecoder(id);
    pushEvt(14, id, id, "video closed", "", pc);
  });
  tr->onFrame([id](rtc::binary data, rtc::FrameInfo) { onVideoFrame(id, data); });
  pushEvt(13, id, id, "video open", "", pc);
}

// ---------- video track setup ----------
// Adds an H.264 SendRecv track to a peer connection.
//
// The five things that silently break video if any one of them is wrong, all
// handled here so that no caller can get them wrong:
//   1. the track must exist BEFORE the offer, or the SDP has no m=video and
//      the peer never negotiates video at all;
//   2. the SSRC and payload type in the SDP must match the packetizer's, or
//      the remote depacketizer discards every packet;
//   3. the clock rate is 90 kHz for video (48 kHz is audio);
//   4. the packetizer needs Separator::StartSequence to match Annex-B input;
//   5. a PliHandler must be in the chain so the remote can request an IDR -
//      the only way to recover from a lost GOP.
void createVideoTrack(int pc) {
  std::shared_ptr<rtc::PeerConnection> cpc = findPc(pc);
  if (!cpc) return;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    if (g_vtrs.find(pc) != g_vtrs.end()) return; // already has one
  }

  const uint32_t ssrc = 0x56000000u + (uint32_t)(GetTickCount() & 0xFFFFu) + (uint32_t)pc * 7919u;

  rtc::Description::Video media("video", rtc::Description::Direction::SendRecv);
  media.addH264Codec(kVideoPT, kH264Profile);
  media.addSSRC(ssrc, "video", "stream0", std::to_string(pc).c_str());

  std::shared_ptr<rtc::Track> tr;
  try {
    tr = cpc->addTrack(std::move(media));
  } catch (...) {
    pushEvt(9, kPcId, 0, "failed to add video track", "", pc);
    return;
  }
  if (!tr) {
    pushEvt(9, kPcId, 0, "addTrack returned no video track", "", pc);
    return;
  }

  const int id = newId();
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_trs[id] = tr;
  }
  if (auto dec = makeDecoderEntry()) {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_vdecs[id] = dec;
    g_vstat[id] = VStat{};
  }
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_trpc[id] = pc;
  }

  auto rtpConfig =
      std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "video", kVideoPT, kVideoClock);
  auto packetizer =
      std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);
  packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
  packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
  // fires when the remote decoder wants a fresh intra frame
  packetizer->addToChain(std::make_shared<rtc::PliHandler>([pc]() {
    g_v_need_idr.store(true);
    g_diag_v_pli.fetch_add(1);
    pushEvt(15, kPcId, 0, "keyframe requested", "", pc);
  }));
  // Inbound side of the same track: without a depacketizer in the chain,
  // incoming RTP is never reassembled into frames and onFrame never fires.
  packetizer->addToChain(
      std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
  tr->setMediaHandler(packetizer);

  tr->onOpen([pc, id] { pushEvt(13, id, id, "video open", "", pc); });
  tr->onClosed([pc, id] { videoTrackClosed(pc, id); });
  tr->onFrame([id](rtc::binary data, rtc::FrameInfo) { onVideoFrame(id, data); });

  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    g_vtrs[pc] = tr;
  }
  g_diag_v_tracks.fetch_add(1);
}

} // namespace

// ---------- pc / dc callbacks ----------
namespace {

void registerDataChannel(std::shared_ptr<rtc::DataChannel> dc, int pc) {
  int id = newId();
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dcs[id] = dc;
    g_dc_last = id;
  }
  dc->onOpen([id] {
    {
      std::lock_guard<std::mutex> lk(g_mtx);
      g_dc_last = id;
    }
    pushEvt(3, id, id, "open", "");
  });
  dc->onClosed([id] { pushEvt(7, id, id, "closed", ""); });
  dc->onError([id](std::string error) { pushEvt(9, id, id, error, ""); });
  dc->onMessage([id, pc](rtc::message_variant data) {
    if (std::holds_alternative<rtc::string>(data)) {
      const auto& s = std::get<rtc::string>(data);
      pushMsg(id, pc, s.data(), (int)s.size());
    } else {
      const auto& b = std::get<rtc::binary>(data);
      pushBin(id, pc, (const char*)b.data(), (int)b.size());
    }
  });
}

} // namespace

// ---------- exported API (__cdecl, for GameMaker dll_cdecl) ----------
extern "C" {

// The self-test further down drives the public API, and some of those calls
// are defined after it, so declare them up front.
double __cdecl net_init(void);
double __cdecl net_create_pc(void);
void __cdecl net_close_pc(double pc);
double __cdecl net_create_offer(double pc);
double __cdecl net_create_answer(double pc);
double __cdecl net_set_remote(double pc, const char *sdp, const char *type);
double __cdecl net_add_ice(double pc, const char *mid, double index, const char *cand);
double __cdecl net_poll(void);
double __cdecl net_event_id(void);
double __cdecl net_event_pc(void);
double __cdecl net_event_int(void);
const char *__cdecl net_event_string(void);
const char *__cdecl net_event_string2(void);
double __cdecl net_video_selftest_ex(double width, double height, double fps, double frames,
                                    const char *dumpName);
double __cdecl net_set_log_level(double level);

__declspec(dllexport) double __cdecl net_version(void) {
  return 4.0;
}

// 0 = silent, 1 = error, 2 = warning (default), 3 = info, 4 = debug.
// libdatachannel logs one line per dropped packet at warning level, which is
// useful while diagnosing and very noisy otherwise.
__declspec(dllexport) double __cdecl net_set_log_level(double level) {
  const int l = (int)level;
  rtc::LogLevel lv = rtc::LogLevel::Warning;
  if (l <= 0) lv = rtc::LogLevel::None;
  else if (l == 1) lv = rtc::LogLevel::Error;
  else if (l == 2) lv = rtc::LogLevel::Warning;
  else if (l == 3) lv = rtc::LogLevel::Info;
  else lv = rtc::LogLevel::Debug;
  rtc::InitLogger(lv);
  return (double)l;
}

__declspec(dllexport) double __cdecl net_init(void) {
  rtc::InitLogger(rtc::LogLevel::Warning);
  rtc::Preload();
  return 1.0;
}

__declspec(dllexport) void __cdecl net_terminate(void) {
  stopAllAudio();
  std::vector<std::shared_ptr<rtc::PeerConnection>> pcs;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& kv : g_pcs) pcs.push_back(kv.second);
    g_pcs.clear();
    g_dcs.clear();
    g_trs.clear();
    g_states.clear();
    g_dc_last = -1;
    g_tr_last = -1;
    g_last_pc = -1;
  }
  for (auto& pc : pcs) pc->close(); // outside the lock
  pcs.clear();                      // Cleanup() waits for PeerConnection destruction
  rtc::Cleanup().get();
}

// Close every peer connection but keep the DLL loaded.
__declspec(dllexport) void __cdecl net_close(void) {
  std::vector<std::shared_ptr<rtc::PeerConnection>> pcs;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& kv : g_pcs) pcs.push_back(kv.second);
    g_pcs.clear();
    g_dcs.clear();
    g_trs.clear();
    g_states.clear();
    g_dc_last = -1;
    g_tr_last = -1;
    g_last_pc = -1;
  }
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_audio_trs.clear();
  }
  // Drop audio engine users so threads can exit cleanly.
  stopAllAudio();
  for (auto& pc : pcs) pc->close(); // outside the lock: close() fires onStateChange -> pushEvt -> re-locks g_mtx
}

__declspec(dllexport) double __cdecl net_add_ice_server(const char* uri) {
  if (!uri || !*uri) return 0.0;
  g_ice_uris.emplace_back(uri);
  return 1.0;
}

// Create a peer connection. useStun adds the configured STUN servers (or
// Google as a fallback); the loopback self-test passes false so the test never
// depends on internet connectivity and cannot be disturbed by a network drop.
static double createPeerConnection(bool useStun) {
  rtc::Configuration cfg;
  if (useStun) {
    for (auto& u : g_ice_uris) cfg.iceServers.emplace_back(u);
    if (cfg.iceServers.empty()) cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
  }
  cfg.enableIceTcp = false;
  cfg.disableAutoNegotiation = true; // GML drives offer/answer via net_create_offer/answer
  std::shared_ptr<rtc::PeerConnection> pc;
  try {
    pc = std::make_shared<rtc::PeerConnection>(cfg);
  } catch (...) {
    return 0.0;
  }

  int pid = newPcId();
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_pcs[pid] = pc;
  }
  g_last_pc.store(pid);
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_states[pid] = 0;
  }

  pc->onStateChange([pid](rtc::PeerConnection::State state) {

    {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_states[pid] = (int)state;
    }
    pushEvt(8, kPcId, (int)state, "", "", pid);
  });
  pc->onLocalDescription([pid](rtc::Description desc) {
    std::string sdp = std::string(desc);
    // Remember which mid carries video so onTrack can tell a video track from
    // an audio one; 0.24.5 exposes no media-kind query on Track.
    std::string vmid;
    if (parseVideoMid(sdp, vmid)) {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_vmid[pid] = vmid;
    }
    pushEvt(1, kPcId, 0, std::move(sdp), desc.typeString(), pid);
  });
  pc->onLocalCandidate([pid](rtc::Candidate cand) {
    pushEvt(2, kPcId, 0, cand.candidate(), cand.mid(), pid);
  });
  pc->onDataChannel([pid](std::shared_ptr<rtc::DataChannel> dc) {
    registerDataChannel(dc, pid);
    int dcid;
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      dcid = g_dc_last;
    }
    pushEvt(6, kPcId, dcid, "dc", "", pid);
  });
  pc->onTrack([pid](std::shared_ptr<rtc::Track> tr) {
    std::string vmid;
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      auto it = g_vmid.find(pid);
      if (it != g_vmid.end()) vmid = it->second;
    }
    if (!vmid.empty() && tr->mid() == vmid)
      handleRemoteVideoTrack(pid, tr);
    else
      handleRemoteTrack(pid, tr); // audio
  });

  return (double)pid;
}

// Close a single peer connection.
__declspec(dllexport) void __cdecl net_close_pc(double id) {
  std::shared_ptr<rtc::PeerConnection> pc;
  bool had_voice = false;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    pc = findPc((int)id);
    if (pc) {
      g_pcs.erase((int)id);
      g_states.erase((int)id);
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (g_audio_trs.erase((int)id) > 0)
      had_voice = true;
  }
  if (had_voice) audioUnref();
  if (pc) pc->close(); // outside the lock: close() may fire callbacks that re-lock
}

__declspec(dllexport) double __cdecl net_create_offer(double pc) {
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  try {
    cpc->setLocalDescription(rtc::Description::Type::Offer);
    return 1.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_create_answer(double pc) {
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  try {
    cpc->setLocalDescription(rtc::Description::Type::Unspec); // auto: answer if remote offer
    return 1.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_set_remote(double pc, const char* sdp, const char* type) {
  if (!sdp || !*sdp) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  std::string t = type ? type : "";
  try {
    cpc->setRemoteDescription(rtc::Description(sdp, t));
    return 1.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_set_local(double pc, const char* sdp, const char* type) {
  // type is the negotiation type ("offer"/"answer", or empty for auto answer);
  // sdp is ignored because local descriptions are generated by the library.
  (void)sdp;
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  rtc::Description::Type t = (type && *type) ? rtc::Description::stringToType(type)
                                             : rtc::Description::Type::Unspec;
  try {
    cpc->setLocalDescription(t);
    return 1.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_add_ice(double pc, const char* mid, double index, const char* cand) {
  (void)index; // libdatachannel derives the m-line from the candidate itself
  if (!cand || !*cand) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  try {
    cpc->addRemoteCandidate(rtc::Candidate(cand, mid ? mid : ""));
    return 1.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_create_dc(double pc, const char* label) {
  std::shared_ptr<rtc::PeerConnection> cpc;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    cpc = findPc((int)pc);
  }
  if (!cpc) return 0.0;
  try {
    auto dc = cpc->createDataChannel(label && *label ? label : "data");
    if (!dc) return 0.0;
    // registerDataChannel() acquires g_mtx itself. Do not hold g_mtx here,
    // otherwise this path deadlocks on a non-recursive std::mutex.
    registerDataChannel(dc, (int)pc);
    int id;
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      id = g_dc_last;
    }
    return (double)id;
  } catch (...) {
    return 0.0;
  }
}

// Create a new peer connection. Returns its pc id (>=1), or 0 on failure.
__declspec(dllexport) double __cdecl net_create_pc(void) { return createPeerConnection(true); }

__declspec(dllexport) double __cdecl net_send(const char* str) {
  if (!str) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_dc_last < 0) return 0.0;
  auto it = g_dcs.find(g_dc_last);
  if (it == g_dcs.end()) return 0.0;
  try {
    return it->second->send(std::string(str)) ? 1.0 : 0.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_send_to(double id, const char* str) {
  if (id < 0 || !str) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  auto it = g_dcs.find((int)id);
  if (it == g_dcs.end()) return 0.0;
  try {
    return it->second->send(std::string(str)) ? 1.0 : 0.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_send_buf(int64_t addr, double size) {
  if (!addr || size <= 0) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_dc_last < 0) return 0.0;
  auto it = g_dcs.find(g_dc_last);
  if (it == g_dcs.end()) return 0.0;
  const char* p = (const char*)addr;
  rtc::binary b(reinterpret_cast<const rtc::byte*>(p),
                reinterpret_cast<const rtc::byte*>(p) + (size_t)size);
  try {
    return it->second->send(std::move(b)) ? 1.0 : 0.0;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_send_buf_to(double id, int64_t addr, double size) {
  if (id < 0 || !addr || size <= 0) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
  auto it = g_dcs.find((int)id);
  if (it == g_dcs.end()) return 0.0;
  const char* p = (const char*)addr;
  rtc::binary b(reinterpret_cast<const rtc::byte*>(p),
                reinterpret_cast<const rtc::byte*>(p) + (size_t)size);
  try {
    return it->second->send(std::move(b)) ? 1.0 : 0.0;
  } catch (...) {
    return 0.0;
  }
}

// Enable/disable the local mic for a specific peer. Voice is per-pc.
__declspec(dllexport) double __cdecl net_voice(double pc, double on) {
  if (on != 0.0) {
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (!findPc((int)pc)) return 0.0;
    }
    bool already = false;
    {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      already = g_audio_trs.find((int)pc) != g_audio_trs.end();
    }
    if (!already) {
      createAudioTrack((int)pc);
      audioRef();
    }
    return 1.0;
  } else {
    bool had = false;
    {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      had = g_audio_trs.erase((int)pc) > 0;
    }
    if (had) audioUnref();
    return 1.0;
  }
}

__declspec(dllexport) double __cdecl net_audio_volume(double v) {
  if (v < 0.0) v = 0.0;
  if (v > 2.0) v = 2.0;
  g_volume.store((float)v);
  return 1.0;
}

__declspec(dllexport) double __cdecl net_audio_mute(double m) {
  g_mic_mute.store(m != 0.0);
  return 1.0;
}

__declspec(dllexport) double __cdecl net_audio_diag(int sel) {
  switch (sel) {
    case 0: return (double)g_diag_encoded.load();
    case 1: return (double)g_diag_sent.load();
    case 2: return (double)g_diag_decoded.load();
    case 3: {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      size_t n = 0;
      for (auto& kv : g_playout) n += kv.second.size();
      return (double)n;
    }
    case 4: {
      std::lock_guard<std::mutex> lk(g_audio_mtx);
      return (double)g_audio_trs.size();
    }
    case 5: return g_audio_running.load() ? 1.0 : 0.0;
    case 6: return (double)g_diag_pb_loops.load();
    case 7: return (double)g_diag_pb_write.load();
    case 8: return (double)g_diag_pb_none.load();
    case 9: return (double)g_diag_pb_pop.load();
    case 10: return (double)g_diag_onframe.load();
    case 11: return (double)g_diag_nodec.load();
    case 12: return (double)g_diag_opusbad.load();
    case 13: return (double)g_diag_trid.load();
    case 14: return (double)g_diag_decs_size.load();
    default: return -1.0;
  }
}

// ------------------------------------------------------------------ video ---

// Add an H.264 video track to a peer connection. Must be called before
// net_create_offer()/net_create_answer() so the video m-line reaches the SDP.
__declspec(dllexport) double __cdecl net_video_enable(double pc, double w, double h, double fps,
                                                     double kbps) {
  (void)w; (void)h; (void)fps; (void)kbps; // encoder tuning lands with the send API
  const int p = (int)pc;
  if (!findPc(p)) return 0.0;
  createVideoTrack(p); // no-op if this pc already has a video track
  std::lock_guard<std::mutex> lk(g_video_mtx);
  return g_vtrs.find(p) != g_vtrs.end() ? 1.0 : 0.0;
}

// Configure the encoder for a peer connection. Call after net_video_enable and
// before the first net_video_send. kbps 0 means "pick a sane default for this
// resolution"; gopSec is the keyframe interval and also bounds how long a
// corruption can last after packet loss.
__declspec(dllexport) double __cdecl net_video_set_params(double pc, double w, double h, double fps,
                                                         double kbps, double gopSec) {
  const int p = (int)pc;
  if (w < 16 || h < 16) return 0.0;
  std::shared_ptr<VSender> s;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vsenders.find(p);
    if (it == g_vsenders.end()) {
      s = std::make_shared<VSender>();
      g_vsenders[p] = s;
    } else {
      s = it->second;
    }
  }
  std::lock_guard<std::mutex> lk(s->mtx);
  s->w = (int)w;
  s->h = (int)h;
  s->fps = (int)(fps > 0 ? fps : 30);
  s->gopSec = (int)(gopSec > 0 ? gopSec : 1);
  if (kbps > 0) {
    s->kbps = (int)kbps;
  } else {
    // ~0.12 bits per pixel per frame is a good 720p60 / 1080p30 starting point
    const double px = (double)s->w * s->h * s->fps;
    s->kbps = (int)std::max(400.0, px * 0.12 / 1000.0);
  }
  s->wantIdr = true;
  return 1.0;
}

// 1 when the previous frame has finished encoding, so the host should send.
// This is how the host decides to drop rather than letting a queue grow.
__declspec(dllexport) double __cdecl net_video_can_send(double pc) {
  std::shared_ptr<VSender> s;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vsenders.find((int)pc);
    if (it == g_vsenders.end()) return 0.0;
    s = it->second;
  }
  // A real answer, from real queue occupancy. This returned 1.0 on every call,
  // so a host polling it before capturing another frame was told "go ahead" even
  // when the encoder was seconds behind - and the frame it then captured
  // overwrote one that had not been encoded.
  if (s->hw) return s->hw->hasRoomForInput() ? 1.0 : 0.0;
  return 1.0;  // software path is synchronous, so it is always ready
}

// Ask the encoder for an IDR on the next frame. Use after a receiver reports
// event 15 (keyframe requested) or when a viewer joins mid-stream.
__declspec(dllexport) double __cdecl net_video_keyframe(double pc) {
  std::shared_ptr<VSender> s;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vsenders.find((int)pc);
    if (it == g_vsenders.end()) return 0.0;
    s = it->second;
  }
  std::lock_guard<std::mutex> lk(s->mtx);
  s->wantIdr = true;
  if (s->enc) s->enc->ForceIntraFrame(true, 0);
  return 1.0;
}

// Hand the host one raw frame. format: 0 = I420, 1 = NV12, 2 = BGRA.
// flags bit 0 forces an IDR for this frame. The buffer is only read during the
// call. Returns 1 if the frame was encoded and sent.
__declspec(dllexport) double __cdecl net_video_send(double pc, int64_t addr, double w, double h,
                                                    double stride, double format, double flags) {
  if (!addr || w < 16 || h < 16) return 0.0;
  const int p = (int)pc;
  std::shared_ptr<rtc::Track> tr;
  std::shared_ptr<VSender> s;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto t = g_vtrs.find(p);
    if (t == g_vtrs.end()) return 0.0;
    tr = t->second;
    auto it = g_vsenders.find(p);
    if (it == g_vsenders.end()) return 0.0;
    s = it->second;
  }
  if (!tr) return 0.0;

  const int W = (int)w, H = (int)h, F = (int)format;
  const int st = stride > 0 ? (int)stride : (F == kFmtBGRA ? W * 4 : W);

  // normalise everything to I420, which is what the encoder wants
  std::vector<uint8_t> y, u, v, bgra;
  const uint8_t *ypl = nullptr, *upl = nullptr, *vpl = nullptr;
  int yStride = W, cStride = W / 2;
  if (F == kFmtBGRA) {
    bgraToI420((const uint8_t *)(intptr_t)addr, W, H, st, y, u, v);
  } else if (F == kFmtNV12) {
    // NV12: full-resolution luma, interleaved CbCr on the second plane
    y.assign(((const uint8_t *)(intptr_t)addr), ((const uint8_t *)(intptr_t)addr) + (size_t)st * H);
    const size_t off = (size_t)st * H;
    const uint8_t *uv = (const uint8_t *)(intptr_t)addr + off;
    const int cw = W / 2, ch = H / 2;
    u.resize((size_t)cw * ch);
    v.resize((size_t)cw * ch);
    for (int j = 0; j < ch; j++) {
      for (int i = 0; i < cw; i++) {
        u[(size_t)j * cw + i] = uv[(size_t)j * W + i * 2 + 0];
        v[(size_t)j * cw + i] = uv[(size_t)j * W + i * 2 + 1];
      }
    }
    cStride = cw;
  } else {
    y.resize((size_t)W * H);
    u.resize((size_t)(W / 2) * (H / 2));
    v.resize((size_t)(W / 2) * (H / 2));
    const uint8_t *src = (const uint8_t *)(intptr_t)addr;
    for (int j = 0; j < H; j++) std::memcpy(&y[(size_t)j * W], src + (size_t)j * st, (size_t)W);
    for (int j = 0; j < H / 2; j++) {
      std::memcpy(&u[(size_t)j * (W / 2)], src + (size_t)st * H + (size_t)j * (W / 2), (size_t)W / 2);
      std::memcpy(&v[(size_t)j * (W / 2)], src + (size_t)st * H + (size_t)cStride * (H / 2) + (size_t)j * (W / 2),
                  (size_t)W / 2);
    }
    cStride = W / 2;
  }
    ypl = y.data(); upl = u.data(); vpl = v.data();

  std::lock_guard<std::mutex> lk(s->mtx);

  // ---------------------------------------------------------------------
  // Hardware path, preferred. Tried once per peer, and any failure is
  // permanent for that peer so the software path below takes over and the
  // host never sees the difference.
  // ---------------------------------------------------------------------
  if (!s->hwTried) {
    s->hwTried = true;
    s->hw.reset(new MfEncoder());
    if (!s->hw->open(W, H, F, s->kbps)) {
      pushEvt(10, kPcId, 0, "hardware encoder unavailable, using OpenH264", "", p);
      s->hw.reset();
    } else {
      pushEvt(10, kPcId, 0, "hardware encoder active", s->hw->name(), p);
    }
  }
  if (s->hw) {
    // The hardware encoder is asynchronous: queue this frame, then ship
    // whatever finished on an earlier one. Both the frame pool and the output
    // queue are bounded and neither discards, so falling behind costs latency
    // - visible as depth, and blockable by the host via net_video_can_send -
    // rather than costing frames.
    if (!s->hw->send(ypl, yStride, upl, vpl, cStride)) {
      // Never ignore this. A refused frame is one the host captured that will
      // never be encoded, which is exactly what must not happen quietly.
      g_diag_v_dropIn.fetch_add(1);
    } else {
      if (g_v_need_idr.exchange(false)) s->hw->forceKeyframe();
      g_diag_v_seqTx.fetch_add(1);
    }

    std::vector<uint8_t> au;
    bool isIdr = false;
    if (s->hw->take(au, isIdr)) {
      s->encoded++;
      // Report the encoder's own mean so net_video_stats reflects the GPU
      // rather than staying at the software figure.
      s->encNs = (uint64_t)(s->hw->encMs() * 1000000.0 * (double)s->encoded);
      if (isIdr) g_diag_v_idr.fetch_add(1);
      g_diag_v_encoded.fetch_add(1);
      if (!tr->isOpen()) {
        s->dropped++;
        return 0.0;
      }
      // Annex-B straight into the StartSequence packetizer, exactly as the
      // software path does. The encoders emit SPS and PPS in band on the IDR,
      // so they travel with the stream and the packetizer is told not to
      // prepend its own copies. rtc::binary is a vector of rtc::byte, which is
      // std::byte rather than unsigned char, so the bytes are converted rather
      // than reinterpreted.
      std::vector<rtc::byte> rb;
      rb.reserve(au.size());
      for (size_t i = 0; i < au.size(); i++) rb.push_back(static_cast<rtc::byte>(au[i]));
      tr->sendFrame(std::move(rb), rtc::FrameInfo(s->ts));
      s->ts += (uint32_t)(kVideoClock / (s->fps > 0 ? s->fps : 30));
      s->sent++;
      g_diag_v_sent.fetch_add(1);
      return 1.0;
    }
    return 0.0;  // nothing finished yet
  }


  if (!s->enc) {
    ISVCEncoder *enc = nullptr;
    if (WelsCreateSVCEncoder(&enc) != 0 || !enc) {
      pushEvt(9, kPcId, 0, "could not create the H.264 encoder", "", p);
      return 0.0;
    }
    int lvl = WELS_LOG_QUIET;
    enc->SetOption(ENCODER_OPTION_TRACE_LEVEL, &lvl);
    SEncParamExt ep;
    std::memset(&ep, 0, sizeof(ep));
    enc->GetDefaultParams(&ep);
    ep.iUsageType = CAMERA_VIDEO_REAL_TIME; // no lookahead, no B-frames
    ep.iPicWidth = W;
    ep.iPicHeight = H;
    ep.iTargetBitrate = s->kbps * 1000;
    ep.iRCMode = RC_BITRATE_MODE;           // quality-capped, predictable bandwidth
    ep.fMaxFrameRate = (float)s->fps;
    ep.uiIntraPeriod = (unsigned int)(s->fps * s->gopSec);
    ep.bEnableFrameSkip = false;             // the host drops frames, not the encoder
    ep.iSpatialLayerNum = 1;
    ep.iEntropyCodingModeFlag = 0;          // CAVLC: the most compatible profile
    ep.sSpatialLayers[0].iVideoWidth = W;
    ep.sSpatialLayers[0].iVideoHeight = H;
    ep.sSpatialLayers[0].fFrameRate = (float)s->fps;
    ep.sSpatialLayers[0].iSpatialBitrate = s->kbps * 1000;
    if (enc->InitializeExt(&ep) != 0) {
      WelsDestroySVCEncoder(enc);
      pushEvt(9, kPcId, 0, "H.264 encoder initialisation failed", "", p);
      return 0.0;
    }
    s->enc = enc;
    s->w = W;
    s->h = H;
    s->ts = 0;
    s->wantIdr = true; // never start on a P-frame
  }

  SSourcePicture pic;
  std::memset(&pic, 0, sizeof(pic));
  pic.iColorFormat = videoFormatI420;
  pic.iPicWidth = W;
  pic.iPicHeight = H;
  pic.iStride[0] = yStride;
  pic.iStride[1] = cStride;
  pic.iStride[2] = cStride;
  pic.pData[0] = (uint8_t *)ypl;
  pic.pData[1] = (uint8_t *)upl;
  pic.pData[2] = (uint8_t *)vpl;
  pic.uiTimeStamp = s->sent * (1000 / (s->fps > 0 ? s->fps : 30));

  SFrameBSInfo *info = (SFrameBSInfo *)calloc(1, sizeof(SFrameBSInfo));
  if (!info) return 0.0;
  const ULONGLONG t0 = GetTickCount64();
  const int rc = s->enc->EncodeFrame(&pic, info);
  s->encNs += GetTickCount64() - t0;
  if (rc != 0) {
    free(info);
    return 0.0;
  }
  std::vector<rtc::byte> au;
  int ftype = -1;
  for (int l = 0; l < info->iLayerNum; l++) {
    SLayerBSInfo &L = info->sLayerInfo[l];
    if (L.iNalCount <= 0 || !L.pBsBuf) continue;
    ftype = (int)L.eFrameType;
    const rtc::byte *p = reinterpret_cast<const rtc::byte *>(L.pBsBuf);
    for (int k = 0; k < L.iNalCount; k++) {
      au.insert(au.end(), p, p + L.pNalLengthInByte[k]);
      p += L.pNalLengthInByte[k];
    }
  }
  free(info);
  if (au.empty()) return 0.0;

  s->encoded++;
  if (ftype == videoFrameTypeIDR) s->wantIdr = false;
  if (ftype == videoFrameTypeIDR) g_diag_v_idr.fetch_add(1);
  g_diag_v_encoded.fetch_add(1);
  if ((int)flags & 1) s->enc->ForceIntraFrame(true, 0);
  // A receiver that lost reference frames asks for a keyframe (PliHandler sets
  // this). Honouring it is the only way the picture recovers from packet loss -
  // without it the decoder stays stuck on a broken chain until the next
  // periodic IDR, which looks like the video freezing.
  if (g_v_need_idr.exchange(false)) {
    s->enc->ForceIntraFrame(true, 0);
    g_diag_v_idr.fetch_add(1);
  }

  if (!tr->isOpen()) {
    s->dropped++;
    return 0.0;
  }
  // Annex-B straight into the StartSequence packetizer
  tr->sendFrame(rtc::binary(au.data(), au.data() + au.size()), rtc::FrameInfo(s->ts));
  s->ts += (uint32_t)(kVideoClock / (s->fps > 0 ? s->fps : 30));
  s->sent++;
  g_diag_v_sent.fetch_add(1);
  return 1.0;
}

// Register a callback for decoded frames on this peer connection. The callback
// runs on a dedicated DLL thread; pass 0 to unregister. The DLL never touches a
// display device - where frames go is entirely the host's decision.
__declspec(dllexport) double __cdecl net_video_set_cb(double pc, net_video_cb cb, double user) {
  const int p = (int)pc;
  std::shared_ptr<VOut> o;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vouts.find(p);
    if (it == g_vouts.end()) {
      o = std::make_shared<VOut>();
      g_vouts[p] = o;
    } else {
      o = it->second;
    }
  }
  {
    std::lock_guard<std::mutex> lk(o->mtx);
    o->cb = cb;
    o->user = user;
    if (cb && !o->run.load()) {
      o->run.store(true);
      o->th = std::thread(videoOutThread, p, o);
    }
  }
  if (!cb) {
    // Drain first, with the callback still installed, so frames the host has
    // already been given are actually delivered before the stream is torn down.
    // The timeout is a backstop: a wedged host callback must not hang the
    // caller of the unregister call.
    for (int i = 0; i < 500 && o->q.size() > 0; ++i) Sleep(2);
    {
      std::lock_guard<std::mutex> lk(o->mtx);
      o->cb = nullptr;
      o->run.store(false);
    }
    // close() releases the thread if it is parked in pop(), and join() is what
    // actually ends it. Unregistering without joining used to leave a live
    // thread and a dangling shared_ptr reference to the next registration.
    o->q.close();
    if (o->th.joinable()) o->th.join();
  }
  return cb ? 1.0 : 0.0;
}

// Video statistics. sel: 0 frames sent  1 frames dropped (track not open)
// 2 delivered to the callback  3 skipped (host fell behind)  4 encode ms last
// 5 encode ms average  6 PLI received  7 live senders
// Which video codec is actually running, and why. Hardware encode is an
// optimisation rather than a requirement, so this is the only way to tell
// whether the GPU is in use or the software fallback took over. Returns the
// path name and writes the reason / encoder name into `out` when supplied.
//   1 = hardware, 0 = OpenH264 software, negative = not sending
__declspec(dllexport) double __cdecl net_video_hw_info(double pc, char *out, double outLen) {
  const int p = (int)pc;
  std::shared_ptr<VSender> s;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vsenders.find(p);
    if (it != g_vsenders.end()) s = it->second;
  }
  const char *msg = "no video sender";
  double r = -1.0;
  if (s) {
    std::lock_guard<std::mutex> lk(s->mtx);
    if (s->hw) {
      r = 1.0;
      msg = s->hw->name();
    } else if (s->enc) {
      r = 0.0;
      msg = "OpenH264 (software)";
    } else {
      msg = "not configured";
    }
  }
  if (out && outLen > 0) {
    strncpy(out, msg, (size_t)outLen - 1);
    out[(int)outLen - 1] = 0;
  }
  return r;
}

__declspec(dllexport) double __cdecl net_video_stats(double pc, double sel) {
  const int p = (int)pc;
  std::shared_ptr<VSender> s;
  std::shared_ptr<VOut> o;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto a = g_vsenders.find(p);
    if (a != g_vsenders.end()) s = a->second;
    auto b = g_vouts.find(p);
    if (b != g_vouts.end()) o = b->second;
  }
  switch ((int)sel) {
    case 0: if (s) { std::lock_guard<std::mutex> lk(s->mtx); return (double)s->sent; } return 0.0;
    case 1: if (s) { std::lock_guard<std::mutex> lk(s->mtx); return (double)s->dropped; } return 0.0;
    case 2: return o ? (double)o->delivered.load() : 0.0;
    case 3: return o ? (double)o->skipped.load() : 0.0;
    case 4: return g_diag_v_encms.load();
    case 5: {
      if (!s) return 0.0;
      std::lock_guard<std::mutex> lk(s->mtx);
      return s->encoded ? (double)s->encNs / 1e6 / (double)s->encoded : 0.0;
    }
    case 6: return (double)g_diag_v_pli.load();
    case 7: { std::lock_guard<std::mutex> lk(g_video_mtx); return (double)g_vsenders.size(); }
    default: return -1.0;
  }
}

// Video counters, for harnesses and host-side stats.
//  0 access units encoded  1 access units sent   2 IDRs sent
//  3 inbound access units  4 pictures decoded   5 decode failures
//  6 PLIs received         7 live video tracks
__declspec(dllexport) double __cdecl net_video_diag(int sel) {
  switch (sel) {
    case 0: return (double)g_diag_v_encoded.load();
    case 1: return (double)g_diag_v_sent.load();
    case 2: return (double)g_diag_v_idr.load();
    case 3: return (double)g_diag_v_rtp.load();
    case 4: return (double)g_diag_v_frames.load();
    case 5: return (double)g_diag_v_decerr.load();
    case 6: return (double)g_diag_v_pli.load();
    case 7: return (double)g_diag_v_tracks.load();
    case 8: {
      std::lock_guard<std::mutex> lk(g_video_mtx);
      int best = -1;
      for (auto &kv : g_vstat)
        if (kv.second.frames > best) best = kv.second.frames;
      return (double)best;
    }
    case 9: return (double)g_diag_v_compared.load();
    case 10: return (double)g_diag_v_mismatch.load();
    case 11: return (double)g_diag_v_baddim.load();
    case 12: return g_diag_v_dev.load();   // worst mean-luma deviation, 0-255
    case 13: return g_diag_v_encms.load(); // worst single-frame encode time (ms)
    case 14: return g_diag_v_kb.load();    // total encoded payload (KB)
    case 15: return g_diag_hwpath.load();  // 1 = hardware encoder was used
    // lossless-pipeline counters
    case 16: return (double)g_diag_v_dropIn.load();
    case 17: return (double)g_diag_v_dropOut.load();
    case 18: return (double)g_diag_v_blocked.load();
    case 19: return (double)g_diag_v_harnessLate.load();
    case 20: return (double)g_diag_v_seqTx.load();
    case 21: return (double)g_diag_v_seqRx.load();
    // Frames accepted but not yet delivered. Because nothing is discarded
    // internally, this is in-flight depth and never loss - a host that wants
    // to prove losslessness waits for it to drain to zero and checks that
    // seqTx advanced by exactly the number of frames it submitted.
    case 22: {
      const double tx = (double)g_diag_v_seqTx.load();
      const double rx = (double)g_diag_v_seqRx.load();
      return tx > rx ? tx - rx : 0.0;
    }
    // Filler submissions the MFT needed in order to keep producing. Their access
    // units are discarded before the host sees them, so this is not loss - but a
    // large number means the encoder is being fed far faster than frames arrive.
    case 23: return g_diag_v_repeats.load();
    case 24: return g_diag_v_submitted.load();
    case 25: return g_diag_v_queued.load();
    default: return -1.0;
  }
}

namespace {

// Result of a reference decode: what each access unit should look like once
// it has been through the codec locally, so the copy that came back over RTP
// can be compared against it.
struct RefTrace {
  std::vector<float> means;   // mean luma per access unit
  int pictures = 0;           // reference decode pictures emitted
};

// Deterministic moving gradient. Real motion, so P-frames are non-trivial and
// a blank result genuinely means something is broken.
void fillPattern(int W, int H, int n, std::vector<uint8_t> &y, std::vector<uint8_t> &u,
                 std::vector<uint8_t> &v) {
  const int off = n * 6;
  for (int j = 0; j < H; j++)
    for (int i = 0; i < W; i++)
      y[(size_t)j * W + i] = (uint8_t)((i * 3 + j * 2 + off) & 0xFF);
  for (int j = 0; j < H / 2; j++)
    for (int i = 0; i < W / 2; i++) {
      u[(size_t)j * (W / 2) + i] = (uint8_t)((128 + i - off) & 0xFF);
      v[(size_t)j * (W / 2) + i] = (uint8_t)((128 + j - off) & 0xFF);
    }
}

// Encode the pattern, send each access unit on the pc's video track, and in
// parallel decode a local copy to form the reference trace. Also dumps the
// bitstream so it can be opened in VLC to prove the encoder on its own.
int selftestPumpVideo(int pc, int W, int H, int fps, int frames, bool forceIdrNext,
                      const char *dumpPath, bool *idrSeen, RefTrace *ref, double *encodeMsOut,
                      double *payloadKB) {
  std::shared_ptr<rtc::Track> tr;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    auto it = g_vtrs.find(pc);
    if (it == g_vtrs.end()) return 0;
    tr = it->second;
  }
  if (!tr) return 0;

  ISVCEncoder *enc = nullptr;
  if (WelsCreateSVCEncoder(&enc) != 0 || !enc) return 0;
  int lvl = WELS_LOG_QUIET;
  enc->SetOption(ENCODER_OPTION_TRACE_LEVEL, &lvl);

  const int kbps = 6000; // generous: this measures transport, not bandwidth
  SEncParamExt ep;
  std::memset(&ep, 0, sizeof(ep));
  enc->GetDefaultParams(&ep);
  ep.iUsageType = CAMERA_VIDEO_REAL_TIME;
  ep.iPicWidth = W;
  ep.iPicHeight = H;
  ep.iTargetBitrate = kbps * 1000;
  ep.iRCMode = RC_BITRATE_MODE;
  ep.fMaxFrameRate = (float)fps;
  ep.uiIntraPeriod = (unsigned int)fps; // one IDR per second
  ep.bEnableFrameSkip = false;           // the host decides drops, not the encoder
  ep.iSpatialLayerNum = 1;
  ep.iMultipleThreadIdc = 1;
  ep.iEntropyCodingModeFlag = 0;
  ep.sSpatialLayers[0].iVideoWidth = W;
  ep.sSpatialLayers[0].iVideoHeight = H;
  ep.sSpatialLayers[0].fFrameRate = (float)fps;
  ep.sSpatialLayers[0].iSpatialBitrate = kbps * 1000;
  if (enc->InitializeExt(&ep) != 0) {
    WelsDestroySVCEncoder(enc);
    return 0;
  }

  std::shared_ptr<VDec> rd = makeDecoderEntry(); // reference decoder for corruption check

  // One reference decode, shared by the main loop and the tail drain, so a frame
  // is held to the same standard no matter which path produced it. Defined here
  // because the drain below reuses the exact same accounting.
  auto referenceDecode = [&](const auto &bytes) {
    if (!rd) return;
    uint8_t *dst[4] = {nullptr, nullptr, nullptr, nullptr};
    int stride[4] = {0, 0, 0, 0};
    int rw = 0, rh = 0;
    if (rd->dec->DecodeFrame(reinterpret_cast<const unsigned char *>(bytes.data()), (int)bytes.size(), dst, stride, rw, rh) == 0 && dst[0]) {
      uint32_t sum = 0;
      for (int j = 0; j < rh; j++) {
        const uint8_t *row = dst[0] + (size_t)j * stride[0];
        for (int i = 0; i < rw; i++) sum += row[i];
      }
      if (ref) ref->means.push_back((float)sum / (float)((size_t)rw * rh));
      if (ref) ref->pictures++;
    }
  };

  FILE *dump = dumpPath ? fopen(dumpPath, "wb") : nullptr;
  std::vector<uint8_t> y((size_t)W * H), u((size_t)W * H / 4), v((size_t)W * H / 4);
  std::vector<rtc::byte> au;
  int sent = 0;
  uint32_t ts = 0;
  double worstEncodeMs = 0.0;
  double payload = 0.0;
  const uint32_t tsStep = (uint32_t)(kVideoClock / (fps > 0 ? fps : 30));
  // Pace to the target frame rate. Blasting frames as fast as the CPU can
  // encode them is not something a real capture source does, and it overruns
  // the transport - which shows up as frames dropped before they are decoded.
  const double frameMs = 1000.0 / (fps > 0 ? fps : 30);
  double due = (double)GetTickCount64();

  // Prefer a hardware encoder so the integrity checks below validate the GPU
  // path. If none can be opened the software encoder is used, so this harness
  // works identically on a machine with no usable GPU.
  std::unique_ptr<MfEncoder> hw;
  g_diag_hwpath.store(0);
  {
    MfEncoder probe;
    if (probe.open(W, H, fps, 2500)) {
      hw.reset(new MfEncoder());
      if (hw->open(W, H, fps, 2500)) g_diag_hwpath.store(1);
      else hw.reset();
    }
  }

  for (int n = 0; n < frames; n++) {
    fillPattern(W, H, n, y, u, v);

    // One of two encoders produces `au`. The hardware path is preferred and the
    // software path stays as the fallback, so the integrity checks below cover
    // whichever one actually ran - which is the point of routing them through
    // the same reference decode and the same RTP send.
    au.clear();
    int ftype = -1;
    if (hw) {
      if (!hw->send(y.data(), W, u.data(), v.data(), W / 2)) {
        // Never ignore this. A refused frame is one the host captured that will
        // never be encoded, which is exactly what must not happen quietly.
        g_diag_v_dropIn.fetch_add(1);
        continue;
      }
      std::vector<uint8_t> raw;
      bool isIdr = false;
      // The encoder is asynchronous, so an access unit may not be ready yet.
      // Keep asking until it is: abandoning it here would make the harness the
      // thing that loses frames, which is precisely the failure the counters are
      // meant to expose. The input pool is small, so the harness cannot run far
      // ahead of the encoder anyway.
      const ULONGLONG until = GetTickCount64() + 1000;
      for (;;) {
        if (hw->take(raw, isIdr)) break;
        if (GetTickCount64() >= until) break;
        Sleep(1);
      }
      if (raw.empty()) {
        g_diag_v_blocked.fetch_add(1);
        continue;
      }
      if (isIdr) ftype = (int)videoFrameTypeIDR;
      au.reserve(raw.size());
      for (size_t i = 0; i < raw.size(); i++) au.push_back(static_cast<rtc::byte>(raw[i]));
      worstEncodeMs = hw->encMs();
      g_diag_hwpath.store(1);
    } else {
      SSourcePicture pic;
      std::memset(&pic, 0, sizeof(pic));
      pic.iColorFormat = videoFormatI420;
      pic.iPicWidth = W;
      pic.iPicHeight = H;
      pic.iStride[0] = W;
      pic.iStride[1] = W / 2;
      pic.iStride[2] = W / 2;
      pic.pData[0] = y.data();
      pic.pData[1] = u.data();
      pic.pData[2] = v.data();
      pic.uiTimeStamp = n * (1000 / (fps > 0 ? fps : 30));

      SFrameBSInfo *info = (SFrameBSInfo *)calloc(1, sizeof(SFrameBSInfo));
      if (!info) break;
      const ULONGLONG t0 = GetTickCount64();
      int rc = enc->EncodeFrame(&pic, info);
      const double encMs = (double)(GetTickCount64() - t0);
      if (encMs > worstEncodeMs) worstEncodeMs = encMs;
      if (rc != 0) {
        free(info);
        break;
      }
      for (int l = 0; l < info->iLayerNum; l++) {
        SLayerBSInfo &L = info->sLayerInfo[l];
        if (L.iNalCount <= 0 || !L.pBsBuf) continue;
        ftype = (int)L.eFrameType;
        const rtc::byte *p = reinterpret_cast<const rtc::byte *>(L.pBsBuf);
        for (int k = 0; k < L.iNalCount; k++) {
          au.insert(au.end(), p, p + L.pNalLengthInByte[k]);
          p += L.pNalLengthInByte[k];
        }
      }
      free(info);
    }
    if (au.empty()) continue;

    if (ftype == videoFrameTypeIDR) {
      g_diag_v_idr.fetch_add(1);
      if (idrSeen) *idrSeen = true;
    }
    g_diag_v_encoded.fetch_add(1);
    if (dump) fwrite(au.data(), 1, au.size(), dump);
    payload += (double)au.size();

    // local reference decode of the very same bytes
    referenceDecode(au);

    if (tr->isOpen()) {
      // A frame sent before the track opens is silently dropped, and so is a
      // P-frame as the very first frame: the receiver has nothing to reference.
      tr->sendFrame(rtc::binary(au.data(), au.data() + au.size()), rtc::FrameInfo(ts));
      g_diag_v_sent.fetch_add(1);
      sent++;
    }
    ts += tsStep;
    due += frameMs;
    const double now = (double)GetTickCount64();
    if (due > now) {
      const double wait = due - now;
      Sleep(wait > 250.0 ? 250 : (DWORD)wait); // cap so a slow frame cannot stall forever
    } else {
      // Behind schedule. Record it instead of quietly resetting, because a
      // harness that hides its own jitter cannot support any conclusion about
      // the frame rate it is measuring.
      g_diag_v_harnessLate.fetch_add(1);
      due = now; // do not accumulate debt
    }
  }

  // Drain the tail before tearing down. Access units still queued when the
  // encoder is destroyed are discarded by close(), and counting that as a lost
  // picture would blame the pipeline for the harness's teardown order. Anything
  // that cannot be drained inside a bounded time is counted as blocked, not
  // quietly forgiven.
  if (hw) {
    std::vector<uint8_t> tailAu;
    bool tailIdr = false;
    const ULONGLONG drainUntil = GetTickCount64() + 3000;
    while (GetTickCount64() < drainUntil) {
      if (!hw->take(tailAu, tailIdr)) { Sleep(1); continue; }
      if (tailIdr) { g_diag_v_idr.fetch_add(1); if (idrSeen) *idrSeen = true; }
      if (dump) fwrite(tailAu.data(), 1, tailAu.size(), dump);
      payload += (double)tailAu.size();
      if (tr->isOpen()) {
        auto *b = reinterpret_cast<const rtc::byte *>(tailAu.data());
        tr->sendFrame(rtc::binary(b, b + tailAu.size()), rtc::FrameInfo(ts));
        ts += tsStep;
        g_diag_v_sent.fetch_add(1);
        sent++;
      }

      g_diag_v_encoded.fetch_add(1);
      referenceDecode(tailAu);
      tailAu.clear();
    }
  }

  // Lossless-pipeline counters, read from the encoder that actually ran so the
  // selftest reports the real discard counts rather than the globals, which
  // only the net_video_send path maintains.
  if (hw) {
    g_diag_v_dropIn.store(hw->droppedIn());
    g_diag_v_dropOut.store(hw->droppedOut());
    g_diag_v_repeats.store(hw->repeated());
    g_diag_v_submitted.store(hw->submitted());
    g_diag_v_queued.store((uint64_t)hw->queued());
  } else {
    g_diag_v_dropIn.store(0);
    g_diag_v_dropOut.store(0);
  }

  hw.reset(); // stop the encode thread before anything else is torn down
  if (forceIdrNext) enc->ForceIntraFrame(true, 0);
  if (dump) fclose(dump);
  if (rd) {
    rd->closed = true;
    if (rd->dec) {
      rd->dec->Uninitialize();
      WelsDestroyDecoder(rd->dec);
      rd->dec = nullptr;
    }
  }
  enc->Uninitialize();
  WelsDestroySVCEncoder(enc);
  if (encodeMsOut) *encodeMsOut = worstEncodeMs;
  if (payloadKB) *payloadKB = payload / 1024.0;
  return sent;
}

} // namespace

// Loopback diagnostic. Builds two peer connections inside this process, wires
// their signaling together, then pushes encoded video from one to the other
// and reports which layers worked as a bitmask:
//   1  the SDP carried an m=video section
//   2  both video tracks opened
//   3  ICE connected (transport works)
//   4  the depacketizer reassembled access units
//   5  the decoder produced pictures
//   6  a decoded picture has the right size and is not blank
//   7  a forced IDR was produced and accepted
// The encoded bitstream is written to selftest.264; if that plays in VLC the
// encoder is innocent and any failure above it is a transport problem.
//
// NOTE: this consumes the event queue and creates peer connections of its
// own. Do not call it while the host is doing its own signaling.
__declspec(dllexport) double __cdecl net_video_selftest(double flags) {
  (void)flags;
  return net_video_selftest_ex((double)kSelftestW, (double)kSelftestH, 30.0, (double)kSelftestFrames,
                               "selftest.264");
}

// Same test at a chosen resolution and frame rate, so 1080p60 can be measured
// on the same footing as the small default. dumpName may be empty to skip the
// bitstream dump.
__declspec(dllexport) double __cdecl net_video_selftest_ex(double width, double height, double fps,
                                                          double frames, const char *dumpName) {
  const int flags = (dumpName && dumpName[0] == 0x21) ? 1 : 0; // leading '!' = verbose
  const int W = (int)width;
  const int H = (int)height;
  const int fr = (int)(fps > 0 ? fps : 30);
  const int nFrames = (int)(frames > 0 ? frames : kSelftestFrames);
  if (W < 16 || H < 16) return 0.0;

  int mask = 0;
  net_init();
  if (!(flags & 1)) rtc::InitLogger(rtc::LogLevel::Error); // quiet unless asked
  g_diag_v_dropIn.store(0);
  g_diag_v_dropOut.store(0);
  g_diag_v_blocked.store(0);
  g_diag_v_harnessLate.store(0);
  g_diag_v_compared.store(0);
  g_diag_v_mismatch.store(0);
  g_diag_v_baddim.store(0);
  g_diag_v_dev.store(0.0);
  g_diag_v_encms.store(0.0);
  g_diag_v_kb.store(0.0);

  const int a = (int)createPeerConnection(false); // no STUN: this test is loopback-only
  const int b = (int)createPeerConnection(false);
  if (a < 1 || b < 1) {
    net_close_pc((double)a);
    net_close_pc((double)b);
    return 0.0;
  }
  // both tracks before the offer, or the video m-line never reaches the SDP
  createVideoTrack(a);
  createVideoTrack(b);

  // ---- signal ----
  net_create_offer((double)a);
  bool gotOffer = false, gotAnswer = false, connected = false;
  const double deadline = (double)GetTickCount64() + 20000.0;
  while ((double)GetTickCount64() < deadline) {
    double t = net_poll();
    if (t == -1.0) {
      Sleep(5);
      continue;
    }
    const int src = (int)net_event_pc();
    const int dst = (src == a) ? b : a;
    if (t == 1.0) {
      // copy immediately: the getters return pointers into shared scratch
      // buffers that the next poll will overwrite
      const std::string s = net_event_string() ? net_event_string() : "";
      const std::string ty = net_event_string2() ? net_event_string2() : "";
      if (src == a && !gotOffer) {
        // bit 1: did our own SDP actually carry video?
        std::string vm;
        if (parseVideoMid(s, vm)) mask |= 1;
        if (net_set_remote((double)b, s.c_str(), ty.c_str()) != 0.0) {
          net_create_answer((double)b);
          gotOffer = true;
        }
      } else if (src == b && !gotAnswer) {
        if (net_set_remote((double)a, s.c_str(), ty.c_str()) != 0.0) gotAnswer = true;
      }
    } else if (t == 2.0) {
      const std::string c = net_event_string() ? net_event_string() : "";
      const std::string m = net_event_string2() ? net_event_string2() : "";
      if (!c.empty()) net_add_ice((double)dst, m.c_str(), 0, c.c_str());
    } else if (t == 8.0) {
      if ((int)net_event_int() == 2) { // connected
        connected = true;
        break;
      }
    } else if (t == 13.0) {
      mask |= 2; // a video track opened
    } else if (t == 9.0) {
      const char *e = net_event_string();
      if (e) pushEvt(9, kPcId, 0, std::string("[selftest] ") + e, "", src);
    }
  }
  if (!gotOffer || !gotAnswer) {
    net_close_pc((double)a);
    net_close_pc((double)b);
    return (double)mask;
  }
  if (connected) mask |= 4 | 8; // bit 2 (connected) and bit 3
  if (!connected) {
    net_close_pc((double)a);
    net_close_pc((double)b);
    return (double)mask;
  }

  // give the receiving track a moment to report open
  const double tOpen = (double)GetTickCount64() + 3000.0;
  while ((double)GetTickCount64() < tOpen) {
    double t = net_poll();
    if (t == 13.0) mask |= 2;
    else if (t == -1.0) Sleep(5);
  }

  // ---- send ----
  const int vBefore = (int)net_video_diag(4);
  const int rBefore = (int)net_video_diag(3);
  bool idrSeen = false;
  RefTrace ref;
  double worstEncodeMs = 0.0, payloadKB = 0.0;
  const int sentCount = selftestPumpVideo(a, W, H, fr, nFrames, true, dumpName, &idrSeen, &ref,
                                          &worstEncodeMs, &payloadKB);

  // ---- wait for delivery of everything we sent ----
  // Waiting for "a few more frames" is not enough: the tail of the stream is
  // still in flight and the counters would be under-read.
  const double tWait = (double)GetTickCount64() + 20000.0;
  while ((double)GetTickCount64() < tWait) {
    double t = net_poll();
    if (t == -1.0) Sleep(2);
    if (net_video_diag(3) - rBefore >= sentCount) break;
  }
  if (net_video_diag(3) > rBefore) mask |= 16; // bit 4: depacketizer produced frames
  if (net_video_diag(4) > vBefore) mask |= 32;  // bit 5: decoder produced pictures

  // ---- bit 6: size correct, not blank, and not corrupted in transit ----
  // The frames that came back over RTP are compared against a local decode of
  // the identical bitstream. A frame mangled on the way (bad NAL splitting,
  // lost reference frame, mis-ordered packets) shows up as a mean that does
  // not line up with its reference.
  int compared = 0, badSize = 0, mismatched = 0;
  double worstDev = 0.0;
  {
    std::lock_guard<std::mutex> lk(g_video_mtx);
    for (auto &kv : g_vstat) {
      VStat &s = kv.second;
      if (s.frames <= 0) continue;
      if (s.w != W || s.h != H) badSize++;
      const size_t n = s.means.size() < ref.means.size() ? s.means.size() : ref.means.size();
      for (size_t i = 0; i < n; i++) {
        const double dev = std::fabs((double)s.means[i] - (double)ref.means[i]);
        if (dev > worstDev) worstDev = dev;
        if (dev > 2.0f) mismatched++; // >2/255 mean shift == visibly wrong
        compared++;
      }
      break;
    }
  }
  g_diag_v_compared.store(compared);
  g_diag_v_mismatch.store(mismatched);
  g_diag_v_baddim.store(badSize);
  g_diag_v_dev.store(worstDev);
  g_diag_v_encms.store(worstEncodeMs);
  g_diag_v_kb.store(payloadKB);
  if (compared > 0 && mismatched == 0 && badSize == 0) mask |= 64; // bit 6
  // bit 7: an IDR was produced (forcing one on request is the recovery path)
  if (idrSeen) mask |= 128;

  net_close_pc((double)a);
  net_close_pc((double)b);
  // Drain a bounded number of leftovers. This must not be an unbounded loop:
  // a closing peer connection can keep producing events, and spinning until
  // the queue stays empty would hang here forever.
  for (int i = 0; i < 4096 && net_poll() != -1.0; ++i) {
  }
  return (double)mask;
}

__declspec(dllexport) double __cdecl net_state(double pc) {

  std::lock_guard<std::mutex> lock(g_mtx);
  auto it = g_states.find((int)pc);
  if (it == g_states.end()) return -1.0;
  return (double)it->second;
}

__declspec(dllexport) double __cdecl net_pc_count(void) {
  std::lock_guard<std::mutex> lock(g_mtx);
  return (double)g_pcs.size();
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

__declspec(dllexport) double __cdecl net_event_pc(void) {
  return g_cur_valid ? (double)g_cur.pc : -1.0;
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

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  // Never join threads or call complex cleanup from DllMain (loader lock).
  // Callers must use net_terminate() / net_close() for orderly shutdown.
  // On PROCESS_DETACH we only set the flag so threads can notice and exit;
  // we deliberately do not join here.
  if (reason == DLL_PROCESS_DETACH) {
    g_audio_running.store(false);
    g_engine_users.store(0);
  }
  return TRUE;
}