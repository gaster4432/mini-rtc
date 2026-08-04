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

#define RTC_STATIC
#include <rtc/rtc.hpp>
#include <opus/opus.h>

#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
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
int g_last_pc = -1;
int g_dc_last = -1;
int g_tr_last = -1;
int g_next_id = 1;
int g_next_pc = 1;
std::vector<std::string> g_ice_uris;

int newId() { return g_next_id++; }
int newPcId() { return g_next_pc++; }

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
  auto cpc = findPc(pc);
  if (!cpc) return;
  std::lock_guard<std::mutex> lk(g_audio_mtx);
  if (g_audio_trs.find(pc) != g_audio_trs.end()) return; // already have one

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
  g_trs[id] = tr;

  // This is a SendRecv track: the peer's audio comes back on this same
  // track's onFrame, so give the local id its own decoder + mixer lane.
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(kSampleRate, kChannels, &err);
  if (err == OPUS_OK && dec) g_decs[id] = dec;

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
    pushEvt(12, id, id, "audio closed", "", pc);
  });
  tr->onFrame([id](rtc::binary data, rtc::FrameInfo info) { onAudioFrame(id, data, info); });

  g_audio_trs[pc] = tr;
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

} // namespace

// ---------- pc / dc callbacks ----------
namespace {

void registerDataChannel(std::shared_ptr<rtc::DataChannel> dc, int pc) {
  int id = newId();
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dcs[id] = dc;
  }
  g_dc_last = id;
  dc->onOpen([id] {
    g_dc_last = id;
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

__declspec(dllexport) double __cdecl net_version(void) {
  return 4.0;
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
    g_audio_trs.clear();
    g_dc_last = -1;
    g_tr_last = -1;
    g_last_pc = -1;
  }
  for (auto& pc : pcs) pc->close(); // outside the lock: close() fires onStateChange -> pushEvt -> re-locks g_mtx
}

__declspec(dllexport) double __cdecl net_add_ice_server(const char* uri) {
  if (!uri || !*uri) return 0.0;
  g_ice_uris.emplace_back(uri);
  return 1.0;
}

// Create a new peer connection. Returns its pc id (>=1), or 0 on failure.
__declspec(dllexport) double __cdecl net_create_pc(void) {
  rtc::Configuration cfg;
  for (auto& u : g_ice_uris) cfg.iceServers.emplace_back(u);
  if (cfg.iceServers.empty()) cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
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
  g_last_pc = pid;
  g_states[pid] = 0;

  pc->onStateChange([pid](rtc::PeerConnection::State state) {
    g_states[pid] = (int)state;
    pushEvt(8, kPcId, (int)state, "", "", pid);
  });
  pc->onLocalDescription([pid](rtc::Description desc) {
    pushEvt(1, kPcId, 0, std::string(desc), desc.typeString(), pid);
  });
  pc->onLocalCandidate([pid](rtc::Candidate cand) {
    pushEvt(2, kPcId, 0, cand.candidate(), cand.mid(), pid);
  });
  pc->onDataChannel([pid](std::shared_ptr<rtc::DataChannel> dc) {
    registerDataChannel(dc, pid);
    pushEvt(6, kPcId, g_dc_last, "dc", "", pid);
  });
  pc->onTrack([pid](std::shared_ptr<rtc::Track> tr) {
    handleRemoteTrack(pid, tr);
  });

  return (double)pid;
}

// Close a single peer connection.
__declspec(dllexport) void __cdecl net_close_pc(double id) {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    pc = findPc((int)id);
    if (pc) g_pcs.erase((int)id);
  }
  if (pc) pc->close(); // outside the lock
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
  std::lock_guard<std::mutex> lock(g_mtx);
  auto cpc = findPc((int)pc);
  if (!cpc) return 0.0;
  try {
    auto dc = cpc->createDataChannel(label && *label ? label : "data");
    if (!dc) return 0.0;
    registerDataChannel(dc, (int)pc);
    return (double)g_dc_last;
  } catch (...) {
    return 0.0;
  }
}

__declspec(dllexport) double __cdecl net_send(const char* str) {
  if (g_dc_last < 0 || !str) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
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
  if (g_dc_last < 0 || !addr || size <= 0) return 0.0;
  std::lock_guard<std::mutex> lock(g_mtx);
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
    createAudioTrack((int)pc); // no-op if the pc already has one
    audioRef();
    return 1.0;
  } else {
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_audio_trs.erase((int)pc);
    }
    audioUnref();
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
    case 4: return g_audio_trs.size();
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
  if (reason == DLL_PROCESS_DETACH) stopAllAudio();
  return TRUE;
}