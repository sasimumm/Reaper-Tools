// Self-check for the NSGT spectrogram: run `build/nsgt_test`.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/nsgt.h"

static Spectrogram analyze(const std::vector<float> &x)
{
  return computeNSGT(NSGTParams(), (long long)x.size(), [&](long long start, int n, float *out) {
    for (int i = 0; i < n; i++)
    {
      long long j = start + i;
      out[i] = j >= 0 && j < (long long)x.size() ? x[j] : 0;
    }
  });
}

int main()
{
  const double sr = NSGTParams().sampleRate;

  // A4 sine at -6 dBFS: the A4 bin reads 0.5 everywhere away from the edges, and is the loudest bin.
  {
    std::vector<float> x((size_t)(10 * sr));
    for (size_t i = 0; i < x.size(); i++) x[i] = 0.5f * (float)sin(2 * M_PI * 440 * i / sr);
    Spectrogram s = analyze(x);
    const int a4 = (int)lround(s.binOfPitch(69));
    double lo = 1e9, hi = 0;
    for (int f = 20; f < s.frames - 20; f++)
    {
      lo = fmin(lo, s.at(f, a4)), hi = fmax(hi, s.at(f, a4));
      for (int b = 0; b < s.bins; b++) assert(s.at(f, b) <= s.at(f, a4) + 1e-6);
    }
    printf("sine: bins=%d frames=%d A4 bin=%d magnitude %.4f..%.4f\n", s.bins, s.frames, a4, lo, hi);
    assert(lo > 0.48 && hi < 0.52);
  }

  // Clicks, including ones near block boundaries: a high bin peaks at the click's frame.
  {
    std::vector<float> x((size_t)(6 * sr));
    const long long clicks[] = {12345, 25000, 28160, 28300, 56320, 77777, 100000};
    for (long long c : clicks) x[c] = 1;
    Spectrogram s = analyze(x);
    const int b = (int)lround(s.binOfPitch(96)); // C7, ~2 kHz: short kernel
    for (long long c : clicks)
    {
      int expect = (int)lround((double)c / s.p.hop), best = expect - 8;
      for (int f = expect - 8; f <= expect + 8; f++)
        if (s.at(f, b) > s.at(best, b)) best = f;
      printf("click at %lld: expected frame %d, peak at %d\n", c, expect, best);
      assert(abs(best - expect) <= 1);
    }
  }

  // Speed on three minutes of noise.
  {
    std::vector<float> x((size_t)(180 * sr));
    for (float &v : x) v = (float)(rand() / (double)RAND_MAX) - 0.5f;
    auto t0 = std::chrono::steady_clock::now();
    Spectrogram s = analyze(x);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("180 s of audio: %.0f ms, %.1f MB\n", ms, s.mag.size() * 4 / 1e6);
  }
  printf("ok\n");
}
