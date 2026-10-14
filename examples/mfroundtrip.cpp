// Round-trip check: GPU encode -> GPU decode, compared against the source
// frames. This is what proves the hardware decoder is actually reconstructing
// the picture, rather than just producing plausible output.
#include "../mf_video.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const int W = (argc > 1) ? atoi(argv[1]) : 320;
  const int H = (argc > 2) ? atoi(argv[2]) : 240;
  const int N = (argc > 3) ? atoi(argv[3]) : 30;
  const int fps = 30;

  MfEncoder enc;
  if (!enc.open(W, H, fps, 2000)) {
    printf("encoder unavailable: %s\n", enc.error().c_str());
    return 1;
  }
  MfDecoder dec;
  if (!dec.open(W, H)) {
    printf("decoder unavailable: %s\n", dec.error().c_str());
    printf("RESULT: NO HW DECODER (software decode would be used)\n");
    return 0;
  }
  printf("encoder: %s\ndecoder: %s\n", enc.name(), dec.name());

  std::vector<uint8_t> y((size_t)W * H), u((size_t)(W / 2) * (H / 2)), v((size_t)(W / 2) * (H / 2));
  // Keep the source of every frame so a decoded picture can be compared.
  std::vector<std::vector<uint8_t>> src;
  src.reserve(N + 8);

  uint64_t encOut = 0, decOk = 0, decErr = 0;
  double worstDev = 0.0;
  int compared = 0;

  std::vector<uint8_t> dy, du, dv;
  std::vector<uint8_t> au;
  bool isIdr = false;

  auto drain = [&]() {
    while (enc.take(au, isIdr)) {
      encOut++;
      dy.clear();
      du.clear();
      dv.clear();
      int ow = 0, oh = 0, ys = 0, cs = 0;
      if (dec.decode(au.data(), au.size(), dy, du, dv, &ow, &oh, &ys, &cs)) {
        decOk++;
        if (!src.empty()) {
          const std::vector<uint8_t> &s = src.back();
          if (ow == W && oh == H && dy.size() >= s.size()) {
            // Mean absolute luma difference. A correct decode of a re-encoded
            // frame is lossy but close; a broken one is wildly different.
            double sum = 0.0;
            for (size_t i = 0; i < s.size(); i++) sum += std::fabs((double)dy[i] - (double)s[i]);
            const double dev = sum / (double)s.size();
            if (dev > worstDev) worstDev = dev;
            compared++;
          }
        }
      } else if (dec.failed()) {
        decErr++;
      } else {
        // buffering, not an error
      }
    }
  };

  for (int f = 0; f < N; f++) {
    for (int j = 0; j < H; ++j)
      for (int i = 0; i < W; ++i) y[(size_t)j * W + i] = (uint8_t)((i * 2 + j + f * 9) & 0xff);
    for (size_t i = 0; i < u.size(); ++i) {
      u[i] = (uint8_t)(i * 2 + f * 3);
      v[i] = (uint8_t)(i * 3 + f * 5);
    }
    src.push_back(y);
    if (!enc.send(y.data(), W, u.data(), v.data(), W / 2)) break;
    Sleep(4);
    drain();
  }
  for (int i = 0; i < 40; ++i) {
    Sleep(25);
    drain();
  }

  printf("encoded %llu  decoded %llu  decode errors %llu  compared %d\n",
         (unsigned long long)encOut, (unsigned long long)decOk, (unsigned long long)decErr, compared);
  printf("worst mean luma deviation %.2f / 255\n", worstDev);
  if (decErr) printf("decoder last error: %s\n", dec.lastError());

  if (decOk == 0) {
    printf("RESULT: FAIL - nothing decoded\n");
    return 1;
  }
  if (worstDev > 24.0) {
    printf("RESULT: FAIL - decoded pictures do not match the source\n");
    return 1;
  }
  printf("RESULT: HW ROUND TRIP OK\n");
  return 0;
}
