// Probe 2: drive the Media Foundation H.264 hardware encoders through a D3D11
// GPU surface - the path that covers NVIDIA NVENC, Intel Quick Sync and AMD VCN,
// discrete and integrated, from one implementation.
//
// This follows Microsoft's "Hardware H.264 encoding using Media Foundation
// Transforms" sample. Five things the first attempt got wrong, all of which
// had to be corrected before any frame could come out:
//
//   1. D3D11CreateDevice was called with no flags. It needs
//      D3D11_CREATE_DEVICE_VIDEO_SUPPORT or the device is not video-capable
//      and every encoder surface is invalid - which surfaced as
//      MF_E_INVALIDMEDIATYPE on the input type and looked like a media type
//      problem the whole way down.
//   2. MFTEnumEx was called with no type filter, so it returned all four
//      hardware encoders, two of which are HEVC-only. Filtering on an H.264
//      output gives the right set directly.
//   3. The output type was first taken from the MFT unmodified, and when that
//      was rejected it was rebuilt without MF_MT_AVG_BITRATE or MF_MT_FRAME_RATE.
//      It must be built by hand with all six attributes; the docs require
//      MF_MT_AVG_BITRATE to be greater than zero.
//   4. The input type set MF_MT_FRAME_SIZE with SetUINT32 and never set
//      MF_MT_FRAME_RATE. Frame size and frame rate are both packed attributes -
//      MFSetAttributeSize and MFSetAttributeRatio - and writing a ratio as a
//      UINT32 is what made every input type look invalid.
//   5. The encode loop called ProcessInput and then polled ProcessOutput. Async
//      MFTs do not work that way at all: they are driven by an event queue,
//      METransformNeedInput and METransformHaveOutput, and nothing completes
//      unless you wait on those.
//
// Two things an earlier note in this file claimed were unreachable are not
// needed at all: the name-keyed bSPSandPPSInBand attribute, and MF_MT_SAMPLE_TYPE.
// The encoders emit SPS and PPS in band on IDRs, and the probe harvests them.
//
// MinGW forward-declares IMFDXGIDeviceManager and never defines it, so the
// interface below is transcribed from the Windows SDK mfobjects.h.
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

// From the Windows SDK; none of these appear in MinGW's MF headers.
static const GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1,
                                 {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
static const GUID kEncMeanBitRate = {0xf7222374, 0x2144, 0x4815,
                                     {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
static const GUID kEncLowLatency = {0x9d3ecd55, 0x89e8, 0x490a,
                                    {0x97, 0x0a, 0x0c, 0x95, 0x48, 0xd5, 0xa5, 0x6e}};
static const GUID kEncRealTime = {0x143a0ff6, 0xa131, 0x43da,
                                  {0xb8, 0x1e, 0x98, 0xfb, 0xb8, 0xec, 0x37, 0x8e}};
static const GUID kMftAsync = {0xf81a699a, 0x649a, 0x497d,
                               {0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a}};
static const GUID kMftAsyncUnlock = {0xe5666d6b, 0x3422, 0x4eb6,
                                     {0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x7}};

// IMFDXGIDeviceManager, transcribed from the Windows SDK mfobjects.h. Despite
// the name it is not a device owner: every method takes a HANDLE naming the GPU
// adapter, and its DXGI device is uninitialised until ResetDevice is called.
struct MfD3D11DeviceManager : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CloseDeviceHandle(HANDLE) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVideoService(HANDLE, REFIID, void **) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice(HANDLE, REFIID, void **, BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenDeviceHandle(HANDLE *) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDevice(IUnknown *, UINT) = 0;
  virtual HRESULT STDMETHODCALLTYPE TestDevice(HANDLE) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice(HANDLE, BOOL) = 0;
};

// Releases the adapter lock and handle however the function leaves.
struct AdapterGuard {
  MfD3D11DeviceManager *m = nullptr;
  HANDLE h = nullptr;
  bool locked = false;
  ~AdapterGuard() {
    if (!m || !h) return;
    if (locked) m->UnlockDevice(h, FALSE);
    m->CloseDeviceHandle(h);
  }
};

// List the NAL types in an Annex-B buffer, so "did the encoder really produce
// something decodable" is answered by the data rather than by a byte count.
static void reportNals(const char *label, const BYTE *p, DWORD len) {
  printf("  %-6s   out bytes=%u", label, len);
  if (!(len > 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
    printf(" NOT ANNEX-B  first=%02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
    return;
  }
  printf(" ANNEX-B nals:");
  int n = 0;
  for (DWORD i = 0; i + 5 < len && n < 16; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
      printf(" %u", p[i + 4] & 0x1f);
      n++;
    }
  }
  printf("\n");
}

static void tryOne(const char *label, IMFActivate *act, int W, int H, int fps) {
  ComPtr<IMFTransform> mft;
  if (FAILED(act->ActivateObject(IID_PPV_ARGS(&mft))) || !mft) return;

  ComPtr<IMFAttributes> attrs;
  mft->GetAttributes(&attrs);
  if (attrs) {
    // Async MFTs answer everything with MF_E_TRANSFORM_ASYNC_LOCKED until this.
    UINT32 isAsync = 0;
    attrs->GetUINT32(kMftAsync, &isAsync);
    if (isAsync) attrs->SetUINT32(kMftAsyncUnlock, TRUE);
    attrs->SetUINT32(kLowLatency, TRUE);
    attrs->SetUINT32(kEncLowLatency, TRUE);
    attrs->SetUINT32(kEncRealTime, TRUE);
    attrs->SetUINT32(kEncMeanBitRate, (UINT32)(2500000));
  }

  // The manager's DXGI device starts uninitialised; OpenDeviceHandle fails with
  // MF_E_DXGI_DEVICE_NOT_INITIALIZED until ResetDevice assigns a device to it.
  UINT resetToken = 0;
  ComPtr<IMFDXGIDeviceManager> rawMgr;
  if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &rawMgr)) || !rawMgr) return;
  auto *mgr = reinterpret_cast<MfD3D11DeviceManager *>(rawMgr.Get());

  ComPtr<ID3D11Device> ownDev;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  HRESULT hrOwn =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                        nullptr, 0, D3D11_SDK_VERSION, &ownDev, &fl, nullptr);
  if (FAILED(hrOwn) || !ownDev) {
    hrOwn = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                              &ownDev, &fl, nullptr);
  }
  printf("  %-6s D3D11CreateDevice(VIDEO_SUPPORT)=0x%08x level=%u.%u\n", label, (unsigned)hrOwn,
         (fl >> 12) & 0xf, (fl >> 8) & 0xf);
  if (FAILED(hrOwn) || !ownDev) return;
  if (FAILED(mgr->ResetDevice(ownDev.Get(), resetToken))) {
    printf("  %-6s ResetDevice failed\n", label);
    return;
  }
  AdapterGuard guard;
  guard.m = mgr;
  HRESULT hrOpen = mgr->OpenDeviceHandle(&guard.h);
  if (FAILED(hrOpen) || !guard.h) {
    printf("  %-6s OpenDeviceHandle=0x%08x\n", label, (unsigned)hrOpen);
    return;
  }
  ComPtr<ID3D11Device> dev;
  HRESULT hrLock = mgr->LockDevice(guard.h, __uuidof(ID3D11Device), (void **)&dev, TRUE);
  guard.locked = SUCCEEDED(hrLock);
  if (FAILED(hrLock) || !dev) {
    printf("  %-6s LockDevice=0x%08x\n", label, (unsigned)hrLock);
    return;
  }
  HRESULT hrSet = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)rawMgr.Get());
  printf("  %-6s SET_D3D_MANAGER=0x%08x %s\n", label, (unsigned)hrSet,
         SUCCEEDED(hrSet) ? "ATTACHED" : "refused");
  if (FAILED(hrSet)) return;

  DWORD inputStreamID = 0, outputStreamID = 0;
  HRESULT hrIds = mft->GetStreamIDs(1, &inputStreamID, 1, &outputStreamID);
  if (hrIds == E_NOTIMPL) {
    inputStreamID = outputStreamID = 0;
  }

  // Output type must be built by hand, with every attribute the encoder
  // requires - notably a non-zero MF_MT_AVG_BITRATE, and frame size and frame
  // rate as packed attributes.
  ComPtr<IMFMediaType> outputType;
  MFCreateMediaType(&outputType);
  outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  outputType->SetUINT32(MF_MT_AVG_BITRATE, 2500000);
  MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, (UINT32)W, (UINT32)H);
  MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
  outputType->SetUINT32(MF_MT_INTERLACE_MODE, 0);  // progressive
  outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  HRESULT hOut = mft->SetOutputType(outputStreamID, outputType.Get(), 0);
  printf("  %-6s SetOutput(hand-built)=0x%08x\n", label, (unsigned)hOut);
  if (FAILED(hOut)) return;

  // Input type: take the MFT's own enumerated type and add the geometry. Both
  // the size and the rate are packed attributes; setting either as a plain
  // UINT32 is what made every input type invalid before.
  ComPtr<IMFMediaType> inputType;
  bool typed = false;
  for (DWORD i = 0; i < 8 && !typed; i++) {
    ComPtr<IMFMediaType> t;
    if (FAILED(mft->GetInputAvailableType(inputStreamID, i, &t)) || !t) break;
    GUID sub;
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    if (IsEqualGUID(sub, MFVideoFormat_NV12) || IsEqualGUID(sub, MFVideoFormat_I420)) {
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, (UINT32)W, (UINT32)H);
      MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
      HRESULT h = mft->SetInputType(inputStreamID, t.Get(), 0);
      printf("  %-6s SetInput[%u] 0x%08x %s\n", label, i, (unsigned)h,
             SUCCEEDED(h) ? "ACCEPTED" : "refused");
      if (SUCCEEDED(h)) {
        inputType = t;
        typed = true;
      }
    }
  }
  if (!typed) {
    printf("  %-6s no input type accepted\n", label);
    return;
  }

  // The encoder input surface: a plain video-capable texture, wrapped as a
  // Media Foundation buffer. fBottomUpWhenLinear is FALSE, as in the sample.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)W;
  td.Height = (UINT)H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_NV12;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  ComPtr<ID3D11Texture2D> encTex, staging;
  HRESULT hrTex = dev->CreateTexture2D(&td, nullptr, &encTex);
  if (FAILED(hrTex) || !encTex) {
    printf("  %-6s CreateTexture2D=0x%08x\n", label, (unsigned)hrTex);
    return;
  }
  D3D11_TEXTURE2D_DESC sd = td;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
    printf("  %-6s staging texture failed\n", label);
    return;
  }
  ComPtr<IMFMediaBuffer> d3dBuf;
  HRESULT hrBuf = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), encTex.Get(), 0, FALSE,
                                            &d3dBuf);
  if (FAILED(hrBuf) || !d3dBuf) {
    printf("  %-6s MFCreateDXGISurfaceBuffer=0x%08x\n", label, (unsigned)hrBuf);
    return;
  }

  ComPtr<ID3D11DeviceContext> ctx;
  dev->GetImmediateContext(&ctx);

  // Async MFTs are driven by their event queue, not by polling ProcessOutput.
  ComPtr<IMFMediaEventGenerator> eventGen;
  if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&eventGen))) || !eventGen) {
    printf("  %-6s no IMFMediaEventGenerator\n", label);
    return;
  }
  mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  int produced = 0, fed = 0;
  const int kMaxFrames = 8;
  while (fed < kMaxFrames) {
    ComPtr<IMFMediaEvent> ev;
    if (FAILED(eventGen->GetEvent(0, &ev)) || !ev) break;
    MediaEventType type;
    if (FAILED(ev->GetType(&type))) break;

    if (type == METransformNeedInput) {
      // A real gradient, so a uniform fill cannot be mistaken for a working
      // encoder and the bitstream size means something.
      D3D11_MAPPED_SUBRESOURCE mp;
      if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        for (int y = 0; y < H; y++) {
          BYTE *row = (BYTE *)mp.pData + (size_t)y * mp.RowPitch;
          for (int x = 0; x < W; x++)
            row[x * 2] = (BYTE)((x * 3 + y * 5 + fed * 11) & 0xff);  // Y
          row[W] = 128;                                             // interleaved UV
        }
        ctx->Unmap(staging.Get(), 0);
      }
      ctx->CopyResource(encTex.Get(), staging.Get());

      ComPtr<IMFSample> sample;
      MFCreateSample(&sample);
      sample->AddBuffer(d3dBuf.Get());
      sample->SetSampleTime(fed * (10000000LL / fps));
      HRESULT hPi = mft->ProcessInput(inputStreamID, sample.Get(), 0);
      if (FAILED(hPi)) {
        printf("  %-6s ProcessInput=0x%08x\n", label, (unsigned)hPi);
        break;
      }
      fed++;
    } else if (type == METransformHaveOutput) {
      MFT_OUTPUT_DATA_BUFFER ob = {};
      ob.dwStreamID = outputStreamID;
      DWORD status = 0;
      HRESULT hPo = mft->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (FAILED(hPo)) {
        printf("  %-6s ProcessOutput=0x%08x\n", label, (unsigned)hPo);
        if (hPo != MF_E_NOTACCEPTING && hPo != MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        continue;
      }
      if (!ob.pSample) continue;
      ComPtr<IMFSample> os;
      os.Attach(ob.pSample);
      ComPtr<IMFMediaBuffer> mb;
      os->ConvertToContiguousBuffer(&mb);
      BYTE *op = nullptr;
      DWORD ol = 0;
      if (SUCCEEDED(mb->Lock(&op, &ol, nullptr))) {
        reportNals(label, op, ol);
        mb->Unlock();
        produced++;
      }
    }
  }
  printf("  %-6s <== %s (%d in, %d out)\n", label,
         produced ? "GPU SURFACE ENCODE WORKS" : "no output", fed, produced);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("MFStartup=0x%08x\n", (unsigned)MFStartup(MF_VERSION, MFSTARTUP_LITE));

  // Filter on an H.264 output. Without this the list includes HEVC-only
  // encoders and the caller has to pick by trial.
  MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate **acts = nullptr;
  UINT32 count = 0;
  HRESULT hrEnum = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                             MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &out,
                             &acts, &count);
  printf("=== H.264 hardware encoders: %s (count=%u) ===\n",
         FAILED(hrEnum) ? "enum failed" : "found", count);
  if (FAILED(hrEnum) || !count) {
    if (acts) CoTaskMemFree(acts);
    return 1;
  }
  for (UINT32 i = 0; i < count; i++) {
    char lbl[16];
    snprintf(lbl, sizeof lbl, "hw[%u]", i);
    tryOne(lbl, acts[i], 1280, 720, 30);
  }
  CoTaskMemFree(acts);
  MFShutdown();
  return 0;
}
