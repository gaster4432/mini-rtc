// What do the hardware video decoders on this machine actually support?
// The H.264 encoder work only became tractable once the media types were read
// off the MFT rather than assumed, and the decoder needs the same treatment.
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

static void fourcc(const GUID &g, char out[5]) {
  out[0] = (char)(g.Data1 & 0xff);
  out[1] = (char)((g.Data1 >> 8) & 0xff);
  out[2] = (char)((g.Data1 >> 16) & 0xff);
  out[3] = (char)((g.Data1 >> 24) & 0xff);
  out[4] = 0;
}

// Unlock the async MFT before asking what it supports: a locked MFT reports no
// available types at all, which is what made these look empty.
static void unlockIfAsync(IMFTransform *m) {
  ComPtr<IMFAttributes> a;
  if (FAILED(m->GetAttributes(&a)) || !a) return;
  UINT32 v = 0;
  a->GetUINT32(MF_TRANSFORM_ASYNC, &v);
  if (v) a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
}

static bool isAsync(IMFTransform *m) {
  ComPtr<IMFAttributes> a;
  if (FAILED(m->GetAttributes(&a)) || !a) return false;
  UINT32 v = 0;
  a->GetUINT32(MF_TRANSFORM_ASYNC, &v);
  return v != 0;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("MFStartup=0x%08x\n\n", (unsigned)MFStartup(MF_VERSION, MFSTARTUP_LITE));

  for (UINT32 flags : {(UINT32)MFT_ENUM_FLAG_HARDWARE, (UINT32)0}) {
    printf("=== VIDEO DECODERS flags=%s ===\n", flags ? "HARDWARE" : "all");
    IMFActivate **acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, nullptr, nullptr, &acts, &count)) ||
        !count) {
      printf("  none\n\n");
      if (acts) CoTaskMemFree(acts);
      continue;
    }
    printf("  count=%u\n", count);
    for (UINT32 i = 0; i < count; i++) {
      ComPtr<IMFTransform> t;
      if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t))) || !t) continue;
      unlockIfAsync(t.Get());
      printf("  [%u] async=%d\n", i, isAsync(t.Get()) ? 1 : 0);

      for (DWORD k = 0; k < 4; k++) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(t->GetInputAvailableType(0, k, &mt)) || !mt) break;
        GUID sub;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        char fc[5];
        fourcc(sub, fc);
        UINT32 fs = 0;
        mt->GetUINT32(MF_MT_FRAME_SIZE, &fs);
        printf("        in [%u] %-5s %ux%u\n", k, fc, fs >> 16, fs & 0xffff);
      }
      for (DWORD k = 0; k < 4; k++) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(t->GetOutputAvailableType(0, k, &mt)) || !mt) break;
        GUID sub;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        char fc[5];
        fourcc(sub, fc);
        UINT32 fs = 0;
        mt->GetUINT32(MF_MT_FRAME_SIZE, &fs);
        printf("        out[%u] %-5s %ux%u\n", k, fc, fs >> 16, fs & 0xffff);
      }
      acts[i]->Release();
    }
    CoTaskMemFree(acts);
    printf("\n");
  }
  MFShutdown();
  return 0;
}
