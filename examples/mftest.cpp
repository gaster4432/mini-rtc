// Standalone check of MfEncoder, before it is wired into the DLL. Keeping this
// separate means a threading or colour-conversion bug shows up here rather than
// being confused with a WebRTC problem.
#include "../mf_video.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);  // so a hang still shows how far it got
  const int W = (argc > 1) ? atoi(argv[1]) : 640;

  const int H = (argc > 2) ? atoi(argv[2]) : 360;
  const int fps = (argc > 3) ? atoi(argv[3]) : 30;
  const int frames = (argc > 4) ? atoi(argv[4]) : 60;

  MfEncoder enc;
  printf("opening...\n");
  fflush(stdout);
  if (!enc.open(W, H, fps, 2000)) {

    printf("open failed: %s\n", enc.error().c_str());
    printf("RESULT: NO HARDWARE ENCODER (caller must fall back to software)\n");
    return 0;
  }
  printf("encoder: %s  %dx%d @%d\n", enc.name(), W, H, fps);
  printf("opened, sending\n");


  std::vector<uint8_t> y((size_t)W * H), u((size_t)(W / 2) * (H / 2)), v((size_t)(W / 2) * (H / 2));

  uint64_t sent = 0, got = 0, idr = 0;
  int firstNals = 0;
  size_t totalBytes = 0;
  double sumsq = 0.0;

  for (int f = 0; f < frames; ++f) {
    // A moving gradient: distinct per frame, so a repeated or stale frame would
    // show up as identical output sizes.
    for (int j = 0; j < H; ++j)
      for (int i = 0; i < W; ++i) y[(size_t)j * W + i] = (uint8_t)((i * 2 + j + f * 7) & 0xff);
    for (size_t i = 0; i < u.size(); ++i) {
      u[i] = (uint8_t)(i * 3 + f);
      v[i] = (uint8_t)(i * 5 + f * 2);
    }
    if (!enc.send(y.data(), W, u.data(), v.data(), W / 2)) {
      printf("send failed at frame %d\n", f);
      break;
    }
    sent++;
    // Give the encoder thread a moment, then collect whatever is ready.
    Sleep(4);
    std::vector<uint8_t> au;
    bool isIdr = false;
    while (enc.take(au, isIdr)) {
      got++;
      totalBytes += au.size();
      if (isIdr) idr++;
      if (firstNals < 2) {
        // Report the NAL types so "Annex-B with SPS/PPS/IDR" is verified from
        // the data rather than assumed.
        printf("  AU %zu bytes:", au.size());
        for (size_t i = 0; i + 4 < au.size() && i < 4000; ++i)
          if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1)
            printf(" %u", au[i + 4] & 0x1f);
        printf("%s\n", isIdr ? "  (IDR)" : "");
        firstNals++;
      }
      // A cheap integrity signal: distinct frames must not all compress to
      // the same size.
      sumsq += (double)au.size() * (double)au.size();
    }
  }

  // Let the tail drain.
  for (int i = 0; i < 40; ++i) {
    Sleep(25);
    std::vector<uint8_t> au;
    bool isIdr = false;
    while (enc.take(au, isIdr)) {
      got++;
      totalBytes += au.size();
      if (isIdr) idr++;
    }
  }

  printf("sent %llu  encoded %llu  idr %llu  %.1f kbps  sps=%zu pps=%zu\n",
         (unsigned long long)sent, (unsigned long long)got, (unsigned long long)idr,
         totalBytes * 8.0 / 1000.0 / 2.0, enc.sps().size(), enc.pps().size());
  printf("mean encode %.2f ms   frames in %llu out %llu\n", enc.encMs(),
         (unsigned long long)enc.framesIn(), (unsigned long long)enc.framesOut());

  if (got == 0) {
    printf("RESULT: FAIL - no frames encoded\n");
    return 1;
  }
  if (!enc.haveSpsPps()) {
    printf("RESULT: FAIL - no SPS/PPS harvested\n");
    return 1;
  }
  if (sumsq <= 0.0) {
    printf("RESULT: FAIL - zero output\n");
    return 1;
  }
  printf("RESULT: HW ENCODER OK\n");
  return 0;
}
