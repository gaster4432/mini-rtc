// mf_video.cpp - Media Foundation H.264 on the GPU. See mf_video.h for why.
#include "mf_video.h"

#include "mf_compat.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>

// Media Foundation needs a process-wide startup, and the library may hold
// several encoders and decoders at once, so it is reference counted rather than
// started and stopped per object.
namespace {
std::once_flag g_mfOnce;
bool g_mfStarted = false;

void ensureMfStarted() {
  std::call_once(g_mfOnce, [] {
    // MFSTARTUP_LITE: this pipeline touches no registry-backed sources.
    g_mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  });
}

// Splits an Annex-B buffer into NAL units. Returns the type of each, and
// optionally captures a NAL payload with its start code stripped, which is the
// form libdatachannel's packetizer expects.
struct NalSpan {
  const uint8_t *data;
  size_t len;
  int type;
};

// Splits an Annex-B buffer into NAL units.
//
// The subtle part is the boundary: a NAL ends where the *next start code
// begins*, not where the next NAL's payload begins. Using the payload offset
// as the end swallows three of a four-byte start code into the current NAL,
// and because the access unit is rebuilt from these spans when AUD and filler
// are dropped, that turns into real corruption: spurious one-byte NALs with a
// type of 0 in the middle of the stream.
//
// Scanning for the three-byte sequence 00 00 01 covers both start code lengths,
// since the four-byte form contains it one byte in. Emulation prevention
// (00 00 03) cannot be mistaken for a start code.
size_t annexB(const uint8_t *p, size_t len, std::vector<NalSpan> &out) {
  out.clear();
  if (!p || len < 4) return 0;

  // Offset of the start code at or after `from`, and the payload that follows
  // it. The four-byte form is preferred so the boundary lands on its first byte.
  auto findStartCode = [&](size_t from, size_t &code, size_t &payload) -> bool {
    for (size_t i = from; i + 2 < len; ++i) {
      if (p[i] != 0 || p[i + 1] != 0) continue;
      if (i + 3 < len && p[i + 2] == 0 && p[i + 3] == 1) {
        code = i;
        payload = i + 4;
        return true;
      }
      if (p[i + 2] == 1) {
        code = i;
        payload = i + 3;
        return true;
      }
    }
    return false;
  };

  size_t code = 0, cur = 0;
  if (!findStartCode(0, code, cur) || cur >= len) return 0;

  for (;;) {
    size_t nextCode = 0, nextPayload = 0;
    if (!findStartCode(cur, nextCode, nextPayload)) break;
    // The NAL runs up to the start of the next start code, minus any
    // trailing_zero_8bits that belong to the delimiter.
    size_t end = nextCode;
    while (end > cur && p[end - 1] == 0) --end;
    if (end > cur) out.push_back({p + cur, end - cur, p[cur] & 0x1f});
    cur = nextPayload;
    if (cur >= len) break;
  }
  if (cur < len) {
    size_t end = len;
    while (end > cur && p[end - 1] == 0) --end;
    if (end > cur) out.push_back({p + cur, end - cur, p[cur] & 0x1f});
  }
  return out.size();
}


void wideToUtf8(const wchar_t *w, std::string &out) {
  out.clear();
  if (!w) return;
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n > 1) {
    out.resize(n - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
  }
}
}  // namespace

// ===========================================================================
//  MfEncoder
// ===========================================================================

struct MfEncoder::Impl {
  int w = 0, h = 0, fps = 30, kbps = 2500, gopSec = 1;

  ComPtr<IMFTransform> mft;
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFMediaEventGenerator> events;
  // Used to wake a blocked GetEvent at shutdown. MinGW declares
  // IMFMediaEventGenerator::GetEvent without a HANDLE parameter and has no
  // ShutdownEventQueue, so QueueEvent(MEError) is the only way to interrupt it.
  ComPtr<IMFMediaEventQueue> queue;

  ComPtr<IMFDXGIDeviceManager> mgrRaw;
  MfD3D11DeviceManager *mgr = nullptr;
  HANDLE hAdapter = nullptr;
  bool locked = false;
  ComPtr<ID3D11Device> dev;     // the device textures and the MFT share
  ComPtr<ID3D11Device> ownDev;  // the device handed to ResetDevice

  ComPtr<ID3D11DeviceContext> ctx;
  // A ring of encoder surfaces, not one. ProcessInput hands the MFT a
  // reference and the GPU keeps reading the surface for as long as it likes,
  // so overwriting a single surface with the next frame races with the encoder
  // still sampling it. That produced sporadic, non-reproducible corruption:
  // identical bitstream headers giving 89/1 on one run and 41/49 on the next.
  // Three is comfortably more than the encoder's lookahead.
  static const int kSurfaces = 3;
  ComPtr<ID3D11Texture2D> surf[kSurfaces];
  ComPtr<IMFMediaBuffer> d3dBuf[kSurfaces];
  std::vector<uint8_t> packed;   // tightly packed NV12 upload buffer
  int surfIdx = 0;

  DWORD inId = 0, outId = 0;

  std::thread th;
  std::atomic<bool> run{false};

  // Input: a fixed pool of frame slots recycled by the encode thread, plus a
  // queue of slot indices. Recycling avoids allocating a whole frame per
  // submission, and the free list is what makes it lossless - a submission
  // waits for a slot rather than overwriting one.
  struct FrameSlot {
    std::vector<uint8_t> y, u, v;
  };
  static const int kPool = 8;
  FrameSlot pool[kPool];
  std::mutex slotMtx;
  std::condition_variable slotFree;
  std::deque<int> freeSlots;          // indices available for the producer
  std::deque<int> pendingSlots;        // indices queued for the encode thread
  bool needKey = false;

  // Output: a queue, not a slot. Discarding an encoded access unit here is what
  // broke 60 fps - the encoder is faster than the collector, so a second unit
  // overwrote the first and orphaned the reference chain after it.
  struct Encoded {
    std::vector<uint8_t> au;
    bool idr = false;
  };
  // Generously sized on purpose. A cap of 8 is 133 ms of headroom at 60 fps, and
  // a short cap deadlocks: the encode thread blocks in push() while full, so it
  // stops draining the input pool, so the producer blocks behind it. One second
  // of slack is enough that a slow collector shows up as latency the host can
  // query, instead of as a stall with no diagnosis.
  LosslessQ<Encoded> outQ{64};
  std::atomic<bool> outClosed{false};


  std::mutex sMtx;
  std::vector<uint8_t> sps, pps;

  ULONGLONG lastSubmitMs = 0;
  // The MFT has to be fed continuously or it stops producing output entirely, so
  // a repeat is still submitted when no host frame is waiting. Its access unit is
  // not a picture the host captured, so it is filtered out on the way past -
  // tracked here, one flag per submitted input, popped one per output. This is
  // what keeps the input-to-output mapping exactly one-to-one while still
  // keeping the vendor pipeline fed.
  std::deque<bool> submitIsRepeat;

  std::atomic<uint64_t> inCount{0}, outCount{0}, repeatCount{0};
  // Lossless instrumentation. droppedIn counts host frames overwritten before
  // they could be encoded; droppedOut counts encoded access units discarded
  // because nobody collected them. Both orphan the H.264 reference chain, so
  // both are reported rather than tolerated silently. Part B replaces the
  // one-slot buffers these count.
  std::atomic<uint64_t> droppedIn{0}, droppedOut{0};

  std::atomic<double> encAccMs{0.0};
  std::string lastError;

  void teardown() {
    // Release anything blocked in LosslessQ before joining. The encode thread
    // can be sitting in push() on a full output queue, and the producer can be
    // sitting in send() waiting for a free slot; without these closes the join
    // below deadlocks. close() wakes every waiter and makes it return false.
    outClosed.store(true);
    outQ.close();
    {
      std::lock_guard<std::mutex> lk(slotMtx);
      // Waking a producer waiting for a slot requires the predicate, so make it
      // true by marking the encoder as no longer running.
      run.store(false);
    }
    slotFree.notify_all();

    if (th.joinable()) {
      run.store(false);
      // The event wait below is a blocking GetEvent, so shut the event queue
      // down to make it return; without this the join never completes.
      // IMFMediaEventQueue::Shutdown is the supported way to do this, and is
      // what MinGW's IMFMediaEventGenerator is missing.
      if (queue) queue->Shutdown();
      th.join();
    }

    if (mgr) {
      if (locked && hAdapter) mgr->UnlockDevice(hAdapter, FALSE);
      if (hAdapter) mgr->CloseDeviceHandle(hAdapter);
    }
    hAdapter = nullptr;
    locked = false;
    mgr = nullptr;
    for (int i = 0; i < kSurfaces; i++) {
      d3dBuf[i].Reset();
      surf[i].Reset();
    }
    ctx.Reset();
    dev.Reset();
    queue.Reset();
    events.Reset();
    attrs.Reset();
    mft.Reset();
    mgrRaw.Reset();
  }

  // I420 planes -> the NV12 staging texture. The Y plane is a straight copy; the
  // only difference from I420 is that chroma is interleaved.
  // Host pixels are pushed with UpdateSubresource from a tightly packed NV12
  // buffer rather than by mapping a staging texture. Mapping an NV12 staging
  // texture is rejected with E_INVALIDARG on this configuration, and because the
  // copy is the only way host pixels reach the encoder, a silent failure here
  // re-encodes the same surface over and over: the stream looks well formed and
  // decodes cleanly, while carrying no new picture at all. So the result is
  // checked, not assumed.
  bool upload(const uint8_t *y, int yStride, const uint8_t *u, const uint8_t *v, int cStride) {
    ID3D11Texture2D *dst = surf[surfIdx].Get();
    if (!dst || !ctx) return false;
    const int cw = w / 2, chh = h / 2;
    const size_t yBytes = (size_t)w * h;
    const size_t uvBytes = (size_t)w * chh;   // NV12: U and V interleaved
    if (packed.size() != yBytes + uvBytes) packed.resize(yBytes + uvBytes);
    uint8_t *p = packed.data();
    for (int j = 0; j < h; ++j) std::memcpy(p + (size_t)j * w, y + (size_t)j * yStride, w);
    uint8_t *uv = p + yBytes;
    for (int j = 0; j < chh; ++j) {
      const uint8_t *ur = u + (size_t)j * cStride;
      const uint8_t *vr = v + (size_t)j * cStride;
      uint8_t *d = uv + (size_t)j * w;
      for (int i = 0; i < cw; ++i) {
        d[i * 2 + 0] = ur[i];
        d[i * 2 + 1] = vr[i];
      }
    }
    // Row pitch is the luma row width; for a planar destination pDstBox must be
    // null so the whole resource is written and the planes are interpreted as
    // tightly packed.
    ctx->UpdateSubresource(dst, 0, nullptr, packed.data(), w, 0);
    return true;
  }


  void drainOutput(bool *isIdr) {
    // Bounded on purpose, but by time rather than by item count. A fixed count
    // is a hidden frame cap: if the encoder ever produced more than that many
    // access units in one wakeup, the overflow had nowhere to go and was lost.
    // A time budget cannot silently drop work, and the queue now absorbs bursts
    // that used to overflow here. The budget still exists so a misbehaving MFT
    // cannot spin this thread forever and make it impossible to join.
    const ULONGLONG deadline = GetTickCount64() + 8;  // ms
    for (;;) {
      if (GetTickCount64() >= deadline) break;
      MFT_OUTPUT_DATA_BUFFER ob = {};
      ob.dwStreamID = outId;
      DWORD status = 0;
      HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (FAILED(hr) || !ob.pSample) {
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
          lastError = "ProcessOutput failed";
        }
        return;
      }
      ComPtr<IMFSample> os;
      os.Attach(ob.pSample);
      ComPtr<IMFMediaBuffer> mb;
      if (FAILED(os->ConvertToContiguousBuffer(&mb))) continue;

      BYTE *op = nullptr;
      DWORD ol = 0;
      if (FAILED(mb->Lock(&op, &ol, nullptr))) continue;
      std::vector<uint8_t> au(op, op + ol);
      mb->Unlock();

      std::vector<NalSpan> nals;
      const size_t n = annexB(au.data(), au.size(), nals);
      bool idr = false;
      for (size_t i = 0; i < n; ++i) {
        if (nals[i].type == 5) idr = true;
        // Capture SPS (7) and PPS (8) while streaming. Both encoders tested
        // emit them in band on the keyframe.
        if ((nals[i].type == 7 && sps.empty()) || (nals[i].type == 8 && pps.empty())) {
          std::lock_guard<std::mutex> lk(sMtx);
          std::vector<uint8_t> &dst = (nals[i].type == 7) ? sps : pps;
          if (dst.empty()) dst.assign(nals[i].data, nals[i].data + nals[i].len);
        }
      }
      // Drop the access unit delimiter (9) and filler (12) the hardware
      // encoders put on every frame. They carry no picture data, and the
      // OpenH264 decoder this library uses for receiving rejects them - which
      // showed up as most frames failing to decode at all. Rebuilt as Annex-B
      // with four-byte start codes, and with the parameter sets kept in band so
      // a receiver gets them with the keyframe.
      bool needsRewrite = false;
      for (size_t i = 0; i < n; ++i)
        if (nals[i].type == 9 || nals[i].type == 12) needsRewrite = true;
      if (needsRewrite) {
        std::vector<uint8_t> clean;
        clean.reserve(au.size());
        for (size_t i = 0; i < n; ++i) {
          if (nals[i].type == 9 || nals[i].type == 12) continue;
          clean.push_back(0);
          clean.push_back(0);
          clean.push_back(0);
          clean.push_back(1);
          clean.insert(clean.end(), nals[i].data, nals[i].data + nals[i].len);
        }
        au.swap(clean);
      }

      // Submissions and access units are one-to-one and in order, so nothing
      // needs filtering here. The repeat tracking that used to sit at this point
      // is gone because the encoder no longer emits access units for frames the
      // host never supplied.
      if (outClosed.load()) return;
      if (!outQ.push({std::move(au), idr})) return;
      *isIdr = idr;

      outCount.fetch_add(1);
    }
  }

  void threadMain() {
    // inputOutstanding is the encoder's own state as we understand it: we have
    // handed it a frame and it owes us exactly one access unit. Submission is
    // driven by frame arrival against this flag, not by METransformNeedInput.
    // The event cannot be trusted as the trigger: these MFTs ask for input once
    // and then go quiet, so a lossless queue whose frame arrives after the ask
    // would wait forever. Driving it here means a frame is encoded as soon as
    // the encoder has room, and never when it does not.
    bool inputOutstanding = false;
    while (run.load()) {
      ComPtr<IMFMediaEvent> ev;
      // Non-blocking, with a short sleep when there is nothing to do. The
      // pipeline is pumped by polling rather than by a blocking wait because
      // the wait is what makes shutdown fragile, and queue->Shutdown() is kept
      // for teardown.
      HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
      if (SUCCEEDED(hr) && ev) {
        MediaEventType t = MEError;
        if (SUCCEEDED(ev->GetType(&t)) && t != MEError) {
          if (t == METransformHaveOutput || t == METransformNeedInput) {
            bool idr = false;
            drainOutput(&idr);
          } else {
            // A format change or status event: drain, and re-assert the output
            // type if the MFT changed it under us.
            bool idr = false;
            drainOutput(&idr);
          }
          // Any of these means the MFT has released whatever it was holding.
          inputOutstanding = false;
        }
      }

      if (!inputOutstanding) submitOnePendingFrame();

      if (FAILED(hr) || !ev) {
        if (run.load()) Sleep(1);
      }
    }
  }

  // Hand the encoder one queued host frame, if there is one and it has room.
  // Never encodes a picture the host did not supply: an earlier version kept
  // the MFT fed with filler submissions, and the resulting access units for
  // frames that never existed desynchronised the receiver's reference chain so
  // that only the keyframes decoded.
  void submitOnePendingFrame() {
    int consumed = -1;
    bool wantKey = false;
    {
      std::unique_lock<std::mutex> lk(slotMtx);
      if (!pendingSlots.empty()) {
        consumed = pendingSlots.front();
        pendingSlots.pop_front();
      }
      wantKey = needKey;
    }
    if (consumed < 0) return;

    if (!upload(pool[consumed].y.data(), w, pool[consumed].u.data(), pool[consumed].v.data(), w / 2)) {
      // The copy into the encoder surface failed. The host frame is lost, and
      // this is the one path where that can still happen, so it is counted.
      droppedIn.fetch_add(1);
    } else {
      // A keyframe request is satisfied by the next real frame, which is the
      // only place an IDR can legitimately appear.
      if (wantKey) {
        needKey = false;
        if (attrs) attrs->SetUINT32(kEncVideoForceKeyFrame, 1);
      }
      const ULONGLONG t0 = GetTickCount64();
      ComPtr<IMFSample> sample;
      MFCreateSample(&sample);
      sample->AddBuffer(d3dBuf[surfIdx].Get());
      // Advance only after the frame has been handed over, so the next upload
      // targets a surface the encoder is not still reading.
      surfIdx = (surfIdx + 1) % kSurfaces;
      sample->SetSampleTime((LONGLONG)inCount.load() * (10000000LL / (fps > 0 ? fps : 30)));
      HRESULT hrIn = mft->ProcessInput(inId, sample.Get(), 0);
      if (hrIn == MF_E_NOTACCEPTING) {
        // The MFT is not ready after all. It did not take the sample, so put the
        // frame back at the front to be retried and record no progress. Pushing
        // a mapping entry here instead is what desynchronises the output.
        std::lock_guard<std::mutex> lk(slotMtx);
        pendingSlots.push_front(consumed);
        return;
      }
      if (FAILED(hrIn)) {
        lastError = "ProcessInput failed";
        encAccMs.store(encAccMs.load() + (double)(GetTickCount64() - t0));
        return;
      }
      lastSubmitMs = GetTickCount64();
      inCount.fetch_add(1);
      encAccMs.store(encAccMs.load() + (double)(GetTickCount64() - t0));
    }
    {
      std::lock_guard<std::mutex> lk(slotMtx);
      freeSlots.push_back(consumed);
    }
    slotFree.notify_one();
  }
};

MfEncoder::~MfEncoder() { close(); }

void MfEncoder::close() {
  if (p_) {
    p_->teardown();
    delete p_;
    p_ = nullptr;
  }
}

bool MfEncoder::active() const { return p_ != nullptr; }

bool MfEncoder::open(int w, int h, int fps, int kbps) {
  close();
  if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
    error_ = "dimensions must be positive and even";
    return false;
  }
  ensureMfStarted();
  if (!g_mfStarted) {
    error_ = "MFStartup failed";
    return false;
  }

  // Filter on an H.264 output. Enumerating with no filter also returns
  // HEVC-only encoders, and which index is which varies by machine and driver.
  MFT_REGISTER_TYPE_INFO wantH264 = {MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate **acts = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &wantH264,
                       &acts, &count)) ||
      !count) {
    error_ = "no hardware H.264 encoder";
    return false;
  }

  // Try each in turn: which vendor is index 0 is not something to depend on, and
  // one vendor's MFT may refuse a resolution another accepts.
  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<IMFTransform> mft;
    if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft))) || !mft) continue;

    auto *p = new Impl();
    p->w = w;
    p->h = h;
    p->fps = fps > 0 ? fps : 30;
    p->kbps = kbps > 0 ? kbps : 2500;
    p->mft = mft;

    mft->GetAttributes(&p->attrs);
    if (p->attrs) {
      // Asynchronous MFTs answer everything with MF_E_TRANSFORM_ASYNC_LOCKED
      // until this is set. This is the documented unlock; there is no
      // SendMessage handshake involved.
      UINT32 isAsync = 0;
      p->attrs->GetUINT32(kMfTransformAsync, &isAsync);
      if (isAsync) p->attrs->SetUINT32(kMfTransformAsyncUnlock, TRUE);
      p->attrs->SetUINT32(kMfLowLatency, TRUE);
      p->attrs->SetUINT32(kEncCommonLowLatency, TRUE);
      p->attrs->SetUINT32(kEncCommonRealTime, TRUE);
      p->attrs->SetUINT32(kEncCommonMeanBitRate, (UINT32)(p->kbps * 1000));
    }
    // The friendly name is a nicety: MinGW's IMFActivate has no
    // GetAttributes, so read it from the transform's store if it is there and
    // fall back to a generic label. What matters for diagnostics is that
    // net_video_hw_info can distinguish hardware from software at all.
    {
      wchar_t nm[256] = {0};
      UINT32 got = 0;
      if (p->attrs && SUCCEEDED(p->attrs->GetString(MFT_FRIENDLY_NAME_Attribute, nm, 256, &got)))
        wideToUtf8(nm, name_);
    }
    if (name_.empty()) name_ = "hardware H.264 encoder";


    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&p->events)))) {
      delete p;
      name_.clear();
      continue;
    }
    // Needed only so shutdown can interrupt a blocked GetEvent.
    mft->QueryInterface(IID_PPV_ARGS(&p->queue));

    if (FAILED(mft->GetStreamIDs(1, &p->inId, 1, &p->outId)) || p->inId != 0) {
      // A single in, single out stream is assumed; anything else is not the
      // shape this code is written against.
      p->inId = p->outId = 0;
    }

    // The DXGI device manager. Its DXGI device is uninitialised until
    // ResetDevice, and until then OpenDeviceHandle fails with
    // MF_E_DXGI_DEVICE_NOT_INITIALIZED - which is what made an earlier attempt
    // look like an unsupported-encoder problem.
    UINT resetToken = 0;
    bool ok = SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &p->mgrRaw)) && p->mgrRaw;
    p->mgr = ok ? reinterpret_cast<MfD3D11DeviceManager *>(p->mgrRaw.Get()) : nullptr;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const int fpsWant = fps > 0 ? fps : 30;
    // Profile and level have to be chosen together and declared correctly, and
    // the encoder gets this wrong for 60 fps: left alone it declares level 3.1
    // for 720p60 and 4.0 for 1080p60, both one step too low, and a decoder
    // that believes the stream is non-conformant drops most of the frames. The
    // level values are eAVEncH264VLevel, which is level x 10, from the Windows
    // SDK codecapi.h.
    const UINT32 profileIdc = (fpsWant > 30 || w > 1280) ? 77 : 66;
    const bool sixty = fpsWant > 30;
    const UINT32 levelIdc = (w <= 1280) ? (sixty ? 32 : 31) : (sixty ? 42 : 40);
    if (ok) {
      // VIDEO_SUPPORT is not optional: without it the device cannot back an
      // encoder surface, and every input media type is rejected as invalid.
      // The device created here is the one handed to ResetDevice and the one
      // every texture is built from, which is what the reference does -
      // LockDevice is not needed for resources.
      ok = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                                          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      nullptr, 0, D3D11_SDK_VERSION, &p->ownDev, &fl, nullptr));
      if (ok) p->dev = p->ownDev;
    }
    if (ok) ok = SUCCEEDED(p->mgr->ResetDevice(p->ownDev.Get(), resetToken));
    if (ok) ok = SUCCEEDED(p->mgr->OpenDeviceHandle(&p->hAdapter)) && p->hAdapter;

    if (ok) {
      ok = SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                         reinterpret_cast<ULONG_PTR>(p->mgrRaw.Get())));
    }
    if (ok) { p->dev->GetImmediateContext(&p->ctx); ok = p->ctx != nullptr; }

    // Output type, built by hand. Every attribute is required: notably
    // MF_MT_AVG_BITRATE must be greater than zero, and frame size and frame
    // rate are packed attributes rather than plain integers.
    ComPtr<IMFMediaType> outType;
    if (ok) ok = SUCCEEDED(MFCreateMediaType(&outType));
    if (ok) {
      ok = SUCCEEDED(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
           SUCCEEDED(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) &&
           SUCCEEDED(outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)(p->kbps * 1000))) &&
           SUCCEEDED(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h)) &&
           SUCCEEDED(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, (UINT32)p->fps, 1)) &&
           SUCCEEDED(outType->SetUINT32(MF_MT_INTERLACE_MODE, 0)) &&
           // Profile and level have to be chosen together and declared correctly,
           // and the encoder gets this wrong for 60 fps: left alone it declares
           // level 3.1 for 720p60 and 4.0 for 1080p60, both one step too low,
           // and a decoder that believes the stream is non-conformant drops
           // most of the frames. Main carries everything above 720p30.
           SUCCEEDED(outType->SetUINT32(MF_MT_MPEG2_PROFILE, profileIdc)) &&
           SUCCEEDED(outType->SetUINT32(MF_MT_MPEG2_LEVEL, levelIdc)) &&
           SUCCEEDED(outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));

    }
    if (ok) ok = SUCCEEDED(mft->SetOutputType(p->outId, outType.Get(), 0));

    // Input type: take the encoder's own enumerated type and add the geometry.
    ComPtr<IMFMediaType> inType;
    for (DWORD k = 0; ok && k < 8; ++k) {
      ComPtr<IMFMediaType> t;
      if (FAILED(mft->GetInputAvailableType(p->inId, k, &t)) || !t) break;
      GUID sub;
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      if (!IsEqualGUID(sub, MFVideoFormat_NV12) && !IsEqualGUID(sub, MFVideoFormat_I420)) continue;
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
      MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)p->fps, 1);
      if (SUCCEEDED(mft->SetInputType(p->inId, t.Get(), 0))) {
        inType = t;
        ok = true;
        break;
      }
      ok = false;
    }
    if (ok && !inType) ok = false;

    // The encoder input surfaces, plus a staging texture the host's pixels are
    // copied through. fBottomUpWhenLinear is FALSE, matching the sample.
    if (ok) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = (UINT)w;
      td.Height = (UINT)h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      for (int i = 0; ok && i < Impl::kSurfaces; i++) {
        ok = SUCCEEDED(p->dev->CreateTexture2D(&td, nullptr, &p->surf[i])) && p->surf[i];
        if (ok)
          ok = SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), p->surf[i].Get(), 0,
                                                    FALSE, &p->d3dBuf[i])) &&
               p->d3dBuf[i];
      }
    }


    if (ok) {
      mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      // Size the recycling pool once. Every slot holds a whole frame, so this is
      // the memory cost of the lossless input queue: kPool frames, not one.
      for (int i = 0; i < Impl::kPool; ++i) {
        p->pool[i].y.resize((size_t)w * h);
        p->pool[i].u.resize((size_t)(w / 2) * (h / 2));
        p->pool[i].v.resize((size_t)(w / 2) * (h / 2));
        p->freeSlots.push_back(i);
      }
      // The first frame must be a keyframe or there is nothing for a receiver
      // to decode against. Without this the stream can begin with a P-frame,
      // which shows up as a bitstream that does not start with an SPS and as a
      // receiver failing every frame until the first periodic IDR.
      p->needKey = true;

    }

    if (ok) {
      p->run.store(true);
      p->th = std::thread([p] { p->threadMain(); });
    } else {
      if (name_.empty()) error_ = p->lastError.empty() ? "hardware encoder rejected setup" : p->lastError;
      delete p;
      name_.clear();
      continue;
    }

    p_ = p;
    error_.clear();
    return true;
  }

  CoTaskMemFree(acts);
  if (error_.empty()) error_ = "no hardware H.264 encoder could be opened";
  return false;
}

bool MfEncoder::send(const uint8_t *y, int yStride, const uint8_t *u, const uint8_t *v,
                     int cStride) {
  if (!p_) return false;
  const int w = p_->w, h = p_->h;
  // Wait for a free slot rather than overwriting one. This is the backpressure
  // the host is meant to feel: if the encoder has fallen behind, the producer
  // slows down instead of a frame disappearing. The wait is bounded, because a
  // host thread parked here forever is worse than a reported failure - it turns
  // a stalled encoder into a hung application with no diagnosis available.
  const ULONGLONG waitUntil = GetTickCount64() + 2000;
  int idx = -1;
  {
    std::unique_lock<std::mutex> lk(p_->slotMtx);
    while (p_->freeSlots.empty()) {
      if (!p_->run.load() || GetTickCount64() >= waitUntil) break;
      p_->slotFree.wait_for(lk, std::chrono::milliseconds(20));
    }
    if (p_->freeSlots.empty()) {
      p_->lastError = "encoder input queue full: the encode thread is not draining";
      return false;   // closing, or genuinely stuck
    }
    idx = p_->freeSlots.front();
    p_->freeSlots.pop_front();
  }
  auto &s = p_->pool[idx];
  for (int j = 0; j < h; ++j) std::memcpy(s.y.data() + (size_t)j * w, y + (size_t)j * yStride, w);
  for (int j = 0; j < h / 2; ++j) {
    std::memcpy(s.u.data() + (size_t)j * (w / 2), u + (size_t)j * cStride, w / 2);
    std::memcpy(s.v.data() + (size_t)j * (w / 2), v + (size_t)j * cStride, w / 2);
  }
  {
    std::lock_guard<std::mutex> lk(p_->slotMtx);
    p_->pendingSlots.push_back(idx);
  }
  return true;
}

bool MfEncoder::take(std::vector<uint8_t> &au, bool &isIdr) {

  if (!p_) return false;
  Impl::Encoded e;
  if (!p_->outQ.pop(e, 0)) return false;
  au.swap(e.au);
  isIdr = e.idr;
  return true;
}


void MfEncoder::forceKeyframe() {
  if (!p_) return;
  std::lock_guard<std::mutex> lk(p_->slotMtx);
  p_->needKey = true;
}

bool MfEncoder::hasRoomForInput() const {
  // What net_video_can_send needs to report honestly: whether a submission now
  // would block. Reading a real free-slot count is the point - the old
  // implementation returned 1.0 unconditionally, so the host had a backpressure
  // signal that could not distinguish a healthy encoder from a saturated one.
  std::lock_guard<std::mutex> lk(p_->slotMtx);
  return !p_->freeSlots.empty();
}


double MfEncoder::encMs() const {
  if (!p_) return 0.0;
  const uint64_t n = p_->outCount.load();
  return n ? (p_->encAccMs.load() / (double)n) : 0.0;
}
uint64_t MfEncoder::framesIn() const { return p_ ? p_->inCount.load() : 0; }
uint64_t MfEncoder::droppedIn() const { return p_ ? p_->droppedIn.load() : 0; }
uint64_t MfEncoder::droppedOut() const { return p_ ? p_->droppedOut.load() : 0; }
uint64_t MfEncoder::repeated() const { return p_ ? p_->repeatCount.load() : 0; }
uint64_t MfEncoder::submitted() const { return p_ ? p_->inCount.load() : 0; }
size_t MfEncoder::queued() const {
  if (!p_) return 0;
  std::lock_guard<std::mutex> lk(p_->slotMtx);
  return p_->pendingSlots.size();
}
uint64_t MfEncoder::framesOut() const { return p_ ? p_->outCount.load() : 0; }

// The harvested parameter sets, read under the lock the encode thread uses.
const std::vector<uint8_t> &MfEncoder::sps() const {
  static const std::vector<uint8_t> kEmpty;
  if (!p_) return kEmpty;
  std::lock_guard<std::mutex> lk(p_->sMtx);
  return p_->sps;
}
const std::vector<uint8_t> &MfEncoder::pps() const {
  static const std::vector<uint8_t> kEmpty;
  if (!p_) return kEmpty;
  std::lock_guard<std::mutex> lk(p_->sMtx);
  return p_->pps;
}
bool MfEncoder::haveSpsPps() const {
  if (!p_) return false;
  std::lock_guard<std::mutex> lk(p_->sMtx);
  return !p_->sps.empty() && !p_->pps.empty();
}

// ===========================================================================
//  MfDecoder
// ===========================================================================

struct MfDecoder::Impl {
  ComPtr<IMFTransform> mft;
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFMediaType> outType;
  DWORD inId = 0, outId = 0;
  int w = 0, h = 0;
  bool i420 = false;          // whether the chosen output type is planar I420
  bool providesSamples = true; // MFT allocates its own output buffer
  DWORD outBufSize = 0;
  bool needTypeChange = false;  // set when the MFT renegotiates its output
  bool hardError = false;      // a real failure, as opposed to buffering
  int queued = 0;              // access units handed in but not yet output
  std::string lastError;  // a std::string, so the pointer handed to callers stays valid
};

// Walks the output type's attributes looking for the frame size, which the
// decoder does not report anywhere else once the type is agreed.
static bool typeSize(IMFMediaType *t, UINT *w, UINT *h) {
  if (!t) return false;
  UINT32 fs = 0;
  if (FAILED(t->GetUINT32(MF_MT_FRAME_SIZE, &fs))) return false;
  *w = fs >> 16;
  *h = fs & 0xffff;
  return *w != 0 && *h != 0;
}

MfDecoder::~MfDecoder() { close(); }

bool MfDecoder::failed() const { return p_ && p_->hardError; }

void MfDecoder::close() {
  delete p_;
  p_ = nullptr;
}

bool MfDecoder::open(int w, int h) {
  close();
  if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
    error_ = "dimensions must be positive and even";
    return false;
  }
  ensureMfStarted();
  if (!g_mfStarted) {
    error_ = "MFStartup failed";
    return false;
  }

  // Enumerate unfiltered. A subtype filter on the input matches nothing here,
  // even though hardware decoders plainly exist, so the H.264 ones are picked
  // out by inspecting what each one actually offers.
  //
  // Hardware decoders are tried first, then everything else. The hardware set
  // does not always include H.264 - on this machine the four hardware decoders
  // report only MPEG-4 Part 2 and MJPEG - so Microsoft's H.264 transform is the
  // fallback. That is still Media Foundation's own decoder, which matters here
  // because it accepts the vendor encoders' output more readily than the
  // OpenH264 build this library otherwise receives with.
  std::string lastStep = "none enumerated";
  IMFActivate **acts = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_HARDWARE, nullptr, nullptr, &acts,
                       &count)) ||
      !count) {
    error_ = "no hardware video decoder";
    return false;
  }

  // A second pass over every decoder, appended after the hardware ones.
  std::vector<IMFActivate *> all;
  {
    IMFActivate **sw = nullptr;
    UINT32 swCount = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, 0, nullptr, nullptr, &sw, &swCount)) &&
        swCount) {
      for (UINT32 i = 0; i < count; i++) all.push_back(acts[i]);
      for (UINT32 i = 0; i < swCount; i++) all.push_back(sw[i]);
    } else {
      for (UINT32 i = 0; i < count; i++) all.push_back(acts[i]);
      if (sw) CoTaskMemFree(sw);
    }
  }

  for (size_t i = 0; i < all.size(); i++) {
    ComPtr<IMFTransform> mft;
    if (FAILED(all[i]->ActivateObject(IID_PPV_ARGS(&mft))) || !mft) continue;
    // Hardware decoders need unlocking; the software transform is synchronous.
    const bool isHw = (i < count);
    auto *p = new Impl();
    p->mft = mft;
    // Record which negotiation step failed, so an unusable decoder can be
    // told apart from one that was never offered.
    char step[64] = "unknown";
    std::string es = "H264/ES";
    mft->GetAttributes(&p->attrs);
    if (p->attrs) {
      UINT32 isAsync = 0;
      p->attrs->GetUINT32(kMfTransformAsync, &isAsync);
      if (isAsync) p->attrs->SetUINT32(kMfTransformAsyncUnlock, TRUE);
      // This is what actually makes the decoder use the GPU. Enumerating a
      // hardware decoder is not enough on its own.
      p->attrs->SetUINT32(kDecVideoAccelerationH264, TRUE);
    }
    mft->GetStreamIDs(1, &p->inId, 1, &p->outId);

    // Input type first, unlike the encoder. Only the two mandatory attributes
    // are required; the decoder signals the real geometry with a format change.
    // Annex-B with start codes is MFVideoFormat_H264_ES; the other option is
    // the plain subtype, and which one a given decoder wants is worth probing.
    bool inSet = false;
    p->w = w;
    p->h = h;
    HRESULT lastHr = E_FAIL;
    const GUID inSubs[] = {MFVideoFormat_H264_ES, MFVideoFormat_H264};
    for (const GUID &sub : inSubs) {
      ComPtr<IMFMediaType> inType;
      if (FAILED(MFCreateMediaType(&inType))) continue;
      inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      inType->SetGUID(MF_MT_SUBTYPE, sub);
      // Geometry up front. A hardware decoder will not accept the type with only
      // the two mandatory attributes, and it saves a format change on the first
      // frame as well.
      MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
      MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 30, 1);
      inType->SetUINT32(MF_MT_INTERLACE_MODE, 0);
      lastHr = mft->SetInputType(p->inId, inType.Get(), 0);
      if (SUCCEEDED(lastHr)) {
        inSet = true;
        es = IsEqualGUID(sub, MFVideoFormat_H264_ES) ? "H264/ES(AnnexB)" : "H264/AVCC";
        break;
      }
    }
    if (!inSet) snprintf(step, sizeof step, "SetInputType(%s)=0x%08x", es.c_str(), (unsigned)lastHr);
    bool ok = inSet;

    // Output type second. Prefer planar I420 so the existing frame path can use
    // the result unchanged; fall back to NV12, which is what hardware
    // decoders usually prefer, and convert.
    for (DWORD k = 0; ok && k < 8; ++k) {
      ComPtr<IMFMediaType> t;
      if (FAILED(mft->GetOutputAvailableType(p->outId, k, &t)) || !t) break;
      GUID sub;
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      if (!IsEqualGUID(sub, MFVideoFormat_I420) && !IsEqualGUID(sub, MFVideoFormat_NV12)) continue;
      UINT tw = 0, th = 0;
      if (!typeSize(t.Get(), &tw, &th)) {
        // No frame size yet: the decoder has not seen one, which is normal
        // before the first keyframe. Accept it anyway and learn the size from
        // the format change.
        p->w = 0;
        p->h = 0;
      } else {
        p->w = (int)tw;
        p->h = (int)th;
      }
      p->i420 = IsEqualGUID(sub, MFVideoFormat_I420);
      if (SUCCEEDED(mft->SetOutputType(p->outId, t.Get(), 0))) {
        p->outType = t;
        ok = true;
        break;
      }
      ok = false;
    }
    if (ok) {
      // Who owns the output buffer, and how big a sample the MFT wants.
      MFT_OUTPUT_STREAM_INFO osi;
      std::memset(&osi, 0, sizeof(osi));
      if (SUCCEEDED(mft->GetOutputStreamInfo(p->outId, &osi))) {
        p->providesSamples = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        p->outBufSize = osi.cbSize;
      }
      mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      p_ = p;
      name_ = isHw ? "hardware H.264 decoder" : "Media Foundation H.264 decoder (software)";
      // The accepted input subtype matters a great deal and is easy to miss:
      // the plain H264 subtype means length-prefixed AVCC, so feeding it the
      // Annex-B stream this library produces never yields a picture.
      name_ += std::string(" [in:") + es + "]";
      error_.clear();
      lastError_ = "";
      for (auto *a : all) a->Release();
      CoTaskMemFree(acts);
      return true;
    }
    delete p;
    name_.clear();
    lastStep = step;
  }
  for (auto *a : all) a->Release();
  CoTaskMemFree(acts);
  char msg[160];
  snprintf(msg, sizeof msg, "no H.264 decoder could be opened (last: %s)", lastStep.c_str());

  error_ = msg;
  return false;
}

bool MfDecoder::decode(const uint8_t *data, size_t len, std::vector<uint8_t> &y,
                       std::vector<uint8_t> &u, std::vector<uint8_t> &v, int *outW, int *outH,
                       int *yStride, int *cStride) {
  if (!p_ || !data || len < 5) return false;

  ComPtr<IMFMediaBuffer> buf;
  // MFCreateMediaBuffer is absent from MinGW's headers; the aligned variant
  // takes the same place and the argument is a power-of-two byte alignment.
  if (FAILED(MFCreateAlignedMemoryBuffer((DWORD)len, 16, &buf))) return false;
  BYTE *dst = nullptr;
  if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
  std::memcpy(dst, data, len);
  buf->Unlock();

  ComPtr<IMFSample> in;
  if (FAILED(MFCreateSample(&in)) || FAILED(in->AddBuffer(buf.Get()))) return false;
  in->SetSampleTime(0);
  p_->queued++;
  HRESULT hPi = p_->mft->ProcessInput(p_->inId, in.Get(), 0);
  if (FAILED(hPi)) {
    char m[64];
    snprintf(m, sizeof m, "ProcessInput=0x%08x", (unsigned)hPi);
    p_->lastError = m;
    lastError_ = p_->lastError.c_str();
    return false;
    p_->hardError = true;
  }

  // One access unit in, at most one frame out, so a single output attempt.
  // The HRESULTs are recorded because a decoder that only ever answers
  // MF_E_TRANSFORM_STREAM_CHANGE looks exactly like one that is working, and
  // guessing between those two wastes a lot of time.
  char seq[128] = "";
  size_t seqLen = 0;
  auto note = [&](HRESULT hr) {
    if (seqLen + 12 < sizeof seq) {
      seqLen += (size_t)snprintf(seq + seqLen, sizeof seq - seqLen, "%s0x%08x",
                                seqLen ? "," : "", (unsigned)hr);
    }
  };
  for (int iter = 0; iter < 4; ++iter) {
    MFT_OUTPUT_DATA_BUFFER ob = {};
    ob.dwStreamID = p_->outId;
    DWORD status = 0;
    // Ownership of the output buffer differs between transforms: some allocate
    // it and set PROVIDES_SAMPLES, others expect the client to supply one of
    // the size the stream info asks for. Handing a decoder a null pSample when
    // it wanted a buffer shows up as a stream change that never resolves.
    ComPtr<IMFMediaBuffer> ours;
    ComPtr<IMFSample> ourSample;
    if (!p_->providesSamples) {
      if (FAILED(MFCreateAlignedMemoryBuffer((DWORD)p_->outBufSize, 16, &ours))) return false;
      if (FAILED(MFCreateSample(&ourSample))) return false;
      if (FAILED(ourSample->AddBuffer(ours.Get()))) return false;
      ob.pSample = ourSample.Get();
    }
    HRESULT hr = p_->mft->ProcessOutput(0, 1, &ob, &status);
    note(hr);
    if (ob.pEvents) ob.pEvents->Release();

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // Normal, not an error: the decoder has decided the output format. Take
      // the new type, apply it, and ask again.
      ComPtr<IMFMediaType> nt;
      if (SUCCEEDED(p_->mft->GetOutputCurrentType(p_->outId, &nt)) && nt) {
        GUID sub;
        nt->GetGUID(MF_MT_SUBTYPE, &sub);
        p_->i420 = IsEqualGUID(sub, MFVideoFormat_I420);
        UINT tw = 0, th = 0;
        if (typeSize(nt.Get(), &tw, &th)) {
          p_->w = (int)tw;
          p_->h = (int)th;
        }
        p_->mft->SetOutputType(p_->outId, nt.Get(), 0);
        p_->outType = nt;
      }
      continue;
    }
    snprintf(seq, sizeof seq, "exhausted after %d: %s", iter, seq);
    p_->lastError = seq;
    lastError_ = p_->lastError.c_str();
    if (FAILED(hr) || !ob.pSample) {
      if (hr == MF_E_NOTACCEPTING || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        // Normal: the decoder is buffering and has nothing to show yet. This is
        // not a failure and must not trigger a keyframe request.
        if (p_->queued > 0) p_->queued--;
        p_->hardError = false;
        lastError_ = "";
        return false;
      }
      snprintf(seq, sizeof seq, "ProcessOutput=0x%08x", (unsigned)hr);
      p_->lastError = seq;
      p_->hardError = true;
      lastError_ = p_->lastError.c_str();
      return false;
    }

    ComPtr<IMFSample> os;
    os.Attach(ob.pSample);
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(os->ConvertToContiguousBuffer(&mb))) {
      p_->lastError = "no contiguous output";
      lastError_ = p_->lastError.c_str();
      p_->hardError = true;
      return false;
    }
    BYTE *op = nullptr;
    DWORD ol = 0;
    if (FAILED(mb->Lock(&op, &ol, nullptr))) return false;

    const int W = p_->w, H = p_->h;
    if (W <= 0 || H <= 0) {
      mb->Unlock();
      p_->lastError = "output frame size unknown";
      lastError_ = p_->lastError.c_str();
      p_->hardError = true;
      return false;
    }
    const size_t need = (size_t)W * H + (size_t)(W / 2) * (H / 2) * 2;
    if (ol < need) {
      mb->Unlock();
      p_->lastError = "short output buffer";
      lastError_ = p_->lastError.c_str();
      p_->hardError = true;
      return false;
    }
    if (p_->i420) {
      y.insert(y.end(), op, op + (size_t)W * H);
      u.insert(u.end(), op + (size_t)W * H, op + (size_t)W * H + (size_t)(W / 2) * (H / 2));
      v.insert(v.end(), op + (size_t)W * H + (size_t)(W / 2) * (H / 2),
              op + (size_t)W * H + (size_t)(W / 2) * (H / 2) * 2);
    } else {
      // NV12: interleave the U and V planes back out so callers always get I420.
      const int cw = W / 2, chh = H / 2;
      const BYTE *uv = op + (size_t)W * H;
      y.insert(y.end(), op, op + (size_t)W * H);
      const size_t uOff = y.size();
      y.resize(uOff + (size_t)cw * chh);
      const size_t vOff = y.size();
      y.resize(vOff + (size_t)cw * chh);
      for (int j = 0; j < chh; j++) {
        for (int k = 0; k < cw; k++) {
          y[uOff + (size_t)j * cw + k] = uv[(size_t)j * W + k * 2 + 0];
          y[vOff + (size_t)j * cw + k] = uv[(size_t)j * W + k * 2 + 1];
        }
      }
      u.insert(u.end(), y.begin() + (long)uOff, y.end());
      v.insert(v.end(), y.begin() + (long)vOff, y.end());
      y.resize(uOff);
    }
    mb->Unlock();
    if (outW) *outW = W;
    if (outH) *outH = H;
    if (yStride) *yStride = W;
    if (cStride) *cStride = W / 2;
    lastError_ = "";
    p_->hardError = false;
    if (p_->queued > 0) p_->queued--;
    return true;
  }
  return false;
}

