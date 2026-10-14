// Diagnostic probe: can the Media Foundation H.264 hardware encoders be driven
// from the headers available on this machine?
//
// Verified by running this on this machine:
//   - 4 hardware encoders and 4 hardware decoders are present. Media Foundation
//     fronts NVENC, Quick Sync and VCN behind one API, so this is the path that
//     covers NVIDIA, Intel and AMD, discrete and integrated, with no vendor SDK.
//     Two of the encoders are HEVC-only; two offer H.264.
//   - The hardware MFTs are V2 async MFTs. Every call, including type queries,
//     answers MF_E_TRANSFORM_ASYNC_LOCKED until MF_TRANSFORM_ASYNC_UNLOCK is set
//     on the MFT. That attribute is the documented unlock; there is no
//     SendMessage or ProcessMessage handshake involved.
//   - The output type must be used exactly as the MFT supplies it. Imposing
//     MF_MT_FRAME_SIZE makes it refuse with MF_E_INVALIDMEDIATYPE, and the
//     supplied type carries no frame size, so the encoder has its own default
//     resolution.
//   - Input types are not enumerated at all and have to be constructed.
//
// Header gaps worked around here, none of which exist in MinGW's MF headers:
//   - the name-keyed IMFAttributes methods (SetUINT32ByName and friends) that
//     would set H.264's bSPSandPPSInBand. Probing for them in the vtable
//     crashes, so instead SPS/PPS are captured from the first output frame.
//   - IMFVideoInterlaceMode values, so progressive is spelled out as 0.
//   - MFCreateMediaBuffer, so MFCreateAlignedMemoryBuffer is used.
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

// From the Windows SDK.
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

static void fourcc(const GUID &g, char out[5]) {
  out[0] = (char)(g.Data1 & 0xff);
  out[1] = (char)((g.Data1 >> 8) & 0xff);
  out[2] = (char)((g.Data1 >> 16) & 0xff);
  out[3] = (char)((g.Data1 >> 24) & 0xff);
  out[4] = 0;
}

// Async MFTs answer every call with MF_E_TRANSFORM_ASYNC_LOCKED until this
// attribute is set, so it has to happen before any type work.
static void prep(IMFTransform *mft) {
  ComPtr<IMFAttributes> a;
  if (FAILED(mft->GetAttributes(&a)) || !a) return;
  a->SetUINT32(kLowLatency, 1);
  a->SetUINT32(kEncLowLatency, 1);
  a->SetUINT32(kEncRealTime, 1);
  a->SetUINT32(kEncMeanBitRate, 2500000);
  UINT32 isAsync = 0;
  a->GetUINT32(kMftAsync, &isAsync);
  if (isAsync) a->SetUINT32(kMftAsyncUnlock, 1);
}

static void enumerate(const char *what, GUID cat) {
  printf("=== %s ===\n", what);
  for (UINT32 flags : {(UINT32)MFT_ENUM_FLAG_HARDWARE, (UINT32)0}) {
    IMFActivate **acts = nullptr;
    UINT32 count = 0;
    MFTEnumEx(cat, flags, nullptr, nullptr, &acts, &count);
    printf("  [%s] count=%u\n", flags ? "hardware" : "all     ", count);
    if (!count) {
      if (acts) CoTaskMemFree(acts);
      continue;
    }
    for (UINT32 i = 0; i < count && i < 8; i++) {
      ComPtr<IMFTransform> t;
      printf("     [%u] %s\n", i,
             (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t))) && t) ? "usable" : "dead");
      acts[i]->Release();
    }
    CoTaskMemFree(acts);
  }
}

static void tryOne(const char *label, IMFActivate *act) {
  ComPtr<IMFTransform> mft;
  if (FAILED(act->ActivateObject(IID_PPV_ARGS(&mft))) || !mft) return;
  prep(mft.Get());

  // H.264 output type, as the MFT supplies it.
  ComPtr<IMFMediaType> outType;
  for (DWORD i = 0; i < 8 && !outType; i++) {
    ComPtr<IMFMediaType> t;
    if (SUCCEEDED(mft->GetOutputAvailableType(0, i, &t)) && t) {
      GUID sub;
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      char fc[5];
      fourcc(sub, fc);
      printf("  %-6s out[%u] %s\n", label, i, fc);
      if (!strcmp(fc, "H264")) outType = t;
    }
  }
  if (!outType) {
    printf("  %-6s no H264 output type\n", label);
    return;
  }
  // Unmodified. Overriding the frame size is what made it refuse.
  HRESULT hOut = mft->SetOutputType(0, outType.Get(), 0);
  printf("  %-6s SetOutput(unmodified)=0x%08x\n", label, (unsigned)hOut);
  if (FAILED(hOut)) return;

  // No input types are enumerated, so construct one and find what is accepted.
  struct { const char *nm; const GUID *sub; } subs[] = {
      {"I420", &MFVideoFormat_I420}, {"NV12", &MFVideoFormat_NV12},
      {"YV12", &MFVideoFormat_YV12}, {"IYUV", &MFVideoFormat_IYUV}};
  const int res[][2] = {{1920, 1080}, {1280, 720}, {1280, 800}, {960, 540},
                        {640, 480},  {640, 360},  {320, 240}};
  ComPtr<IMFMediaType> inType;
  int inW = 0, inH = 0;
  for (auto &c : subs) {
    for (auto &r : res) {
      ComPtr<IMFMediaType> it;
      MFCreateMediaType(&it);
      it->SetGUID(MF_MT_SUBTYPE, *c.sub);
      it->SetUINT32(MF_MT_FRAME_SIZE, ((UINT32)r[0] << 16) | (UINT32)r[1]);
      it->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)(r[0] * r[1] * 3 / 2));
      it->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 0);
      it->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 0);
      it->SetUINT32(MF_MT_INTERLACE_MODE, 0);
      it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      it->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)r[0]);
      if (SUCCEEDED(mft->SetInputType(0, it.Get(), 0))) {
        printf("  %-6s ACCEPTED input %s %dx%d\n", label, c.nm, r[0], r[1]);
        inType = it;
        inW = r[0];
        inH = r[1];
        break;
      }
    }
    if (inType) break;
  }
  if (!inType) {
    printf("  %-6s no input accepted (%zu subtypes x %zu sizes)\n", label,
           sizeof(subs) / sizeof(subs[0]), sizeof(res) / sizeof(res[0]));
    return;
  }
  printf("  %-6s <== USABLE %dx%d\n", label, inW, inH);

  // Feed real gradients, so the bitstream size means something and a uniform
  // fill cannot be mistaken for a working encoder.
  const DWORD frameBytes = (DWORD)(inW * inH * 3 / 2);
  for (int f = 0; f < 3; f++) {
    ComPtr<IMFMediaBuffer> buf;
    MFCreateAlignedMemoryBuffer(frameBytes, 16, &buf);
    BYTE *ptr = nullptr;
    buf->Lock(&ptr, nullptr, nullptr);
    for (int y = 0; y < inH; y++)
      for (int x = 0; x < inW; x++)
        ptr[(size_t)y * inW + x] = (BYTE)((x * 3 + y * 5 + f * 11) & 0xff);
    for (size_t i = (size_t)inW * inH; i < frameBytes; i++) ptr[i] = 128;
    buf->Unlock();
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(f * 33333);
    printf("         frame %d ProcessInput=0x%08x\n", f,
           (unsigned)mft->ProcessInput(0, sample.Get(), 0));

    for (int i = 0; i < 4; i++) {
      MFT_OUTPUT_DATA_BUFFER ob = {};
      ob.dwStreamID = 0;
      DWORD status = 0;
      HRESULT hPo = mft->ProcessOutput(0, 1, &ob, &status);
      if (FAILED(hPo) || !ob.pSample) {
        printf("         ProcessOutput=0x%08x (drained)\n", (unsigned)hPo);
        break;
      }
      ComPtr<IMFSample> os;
      os.Attach(ob.pSample);
      ComPtr<IMFMediaBuffer> mb;
      os->ConvertToContiguousBuffer(&mb);
      BYTE *op = nullptr;
      DWORD ol = 0;
      if (SUCCEEDED(mb->Lock(&op, &ol, nullptr))) {
        const bool annexb = ol > 4 && op[0] == 0 && op[1] == 0 && op[2] == 0 && op[3] == 1;
        printf("         out bytes=%u %s", ol, annexb ? "ANNEX-B" : "AVCC/other");
        if (annexb) {
          printf(" nals:");
          for (DWORD p = 0; p + 5 < ol; p++)
            if (!op[p] && !op[p + 1] && !op[p + 2] && op[p + 3] == 1) printf(" %u", op[p + 4] & 0x1f);
        }
        printf("\n");
        mb->Unlock();
      }
    }
  }
}

static void tryEncode() {
  printf("=== hardware H.264 encoders ===\n");
  IMFActivate **acts = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, nullptr, nullptr,
                       &acts, &count)) ||
      !count) {
    printf("  no hardware encoder\n");
    return;
  }
  for (UINT32 i = 0; i < count; i++) {
    char lbl[16];
    snprintf(lbl, sizeof lbl, "hw[%u]", i);
    tryOne(lbl, acts[i]);
  }
  CoTaskMemFree(acts);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // so a crash still shows what ran
  printf("MFStartup=0x%08x\n\n", (unsigned)MFStartup(MF_VERSION, MFSTARTUP_LITE));
  enumerate("VIDEO ENCODERS", MFT_CATEGORY_VIDEO_ENCODER);
  printf("\n");
  enumerate("VIDEO DECODERS", MFT_CATEGORY_VIDEO_DECODER);
  printf("\n");
  tryEncode();
  MFShutdown();
  return 0;
}
