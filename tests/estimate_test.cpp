// Self-check for the note estimator and basic-pitch: run `build/estimate_test`.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

#include "../src/estimate.h"

static const double SR = 22050;

// Voice-like tones (from transcribe's mf0.synth): partials 1..8 at h^-0.6, slight vibrato.
static void tone(std::vector<float> &x, double a, double b, double midi, float amp = 0.1f)
{
  srand(1);
  const double f0 = 440 * pow(2, (midi - 69) / 12);
  double phase[9] = {};
  for (int h = 1; h <= 8; h++) phase[h] = rand() / (double)RAND_MAX * 6.3;
  double ph = 0;
  for (long long i = (long long)(a * SR); i < (long long)(b * SR) && i < (long long)x.size(); i++)
  {
    const double t = i / SR, env = std::min(1.0, std::min(t - a, b - t) / 0.02);
    ph += 2 * M_PI * f0 * (1 + 0.004 * sin(2 * M_PI * 5.5 * t)) / SR;
    double v = 0;
    for (int h = 1; h <= 8; h++) v += sin(h * ph + phase[h]) * pow(h, -0.6);
    x[i] += (float)(amp * env * v);
  }
}

static Spectrogram analyze(const std::vector<float> &x)
{
  return computeNSGT(estimateNSGTParams(), (long long)x.size(), [&](long long st, int n, float *o) {
    for (int i = 0; i < n; i++) o[i] = st + i >= 0 && st + i < (long long)x.size() ? x[st + i] : 0;
  });
}

// The k strongest local maxima of a roll's mean over frames f0..f1, as MIDI pitches.
static std::vector<int> peaks(const std::vector<float> &roll, int lo, int n, int f0, int f1, int k)
{
  std::vector<double> m(n, 0);
  for (int f = f0; f < f1; f++)
    for (int p = 0; p < n; p++) m[p] += roll[(size_t)f * n + p];
  std::vector<int> idx;
  for (int p = 1; p + 1 < n; p++)
    if (m[p] >= m[p - 1] && m[p] > m[p + 1]) idx.push_back(p);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return m[a] > m[b]; });
  idx.resize(std::min((int)idx.size(), k));
  for (int &p : idx) p += lo;
  std::sort(idx.begin(), idx.end());
  return idx;
}

int main()
{
  const int lo = 40, hi = 86, n = hi - lo + 1;

  // NMF finds chord notes without a phantom subharmonic (transcribe's mf0 voicings, incl. the E voicing)
  for (std::vector<int> v : {std::vector<int>{66, 71, 75}, {66, 70, 73}, {64, 68, 71}, {59, 64, 68}})
  {
    std::vector<float> x((size_t)(1.0 * SR));
    for (int m : v) tone(x, 0, 1, m);
    const Spectrogram s = analyze(x);
    const std::vector<int> got = peaks(nmfRoll(s, lo, hi, false), lo, n, 20, s.frames - 20, (int)v.size());
    printf("nmf %d %d %d -> %d %d %d\n", v[0], v[1], v[2], got[0], got[1], got[2]);
    assert(got == v);
  }

  // SACF: the strongest semitone of a single tone is the tone, and its sub-octave stays weak
  {
    std::vector<float> x((size_t)(1.0 * SR));
    tone(x, 0, 1, 57);
    const int frames = (int)(x.size() / 256) + 1;
    const std::vector<float> r = sacfRoll(x, frames, lo, hi);
    const std::vector<int> got = peaks(r, lo, n, 20, frames - 20, 1);
    double at = 0, sub = 0;
    for (int f = 20; f < frames - 20; f++) at += r[(size_t)f * n + 57 - lo], sub += r[(size_t)f * n + 45 - lo];
    printf("sacf A3 -> %d, sub-octave %.2f of it\n", got[0], sub / at);
    assert(got[0] == 57 && sub < 0.3 * at);
  }

  // basic-pitch: posteriors at the right pitch and time, also in a later window (alignment)
  std::vector<float> x((size_t)(8 * SR));
  for (float &v : x) v = 0.0005f * ((float)(rand() / (double)RAND_MAX) - 0.5f);
  tone(x, 1.0, 2.0, 60), tone(x, 1.5, 2.5, 64), tone(x, 4.5, 5.5, 67), tone(x, 6.0, 6.8, 55);
  const Spectrogram s = analyze(x);
  BasicPitch bp;
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  const bool ok = runBasicPitch(BASIC_PITCH_MODEL, x, s.frames, bp, &err);
  printf("basic-pitch: %s %.0f ms for 8 s\n", ok ? "ok" : err.c_str(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  assert(ok && bp.frames == s.frames);
  const double fr = s.frameRate();
  for (auto c : {std::make_pair(1.0, 60), std::make_pair(1.5, 64), std::make_pair(4.5, 67), std::make_pair(6.0, 55)})
  {
    const int expect = (int)lround(c.first * fr);
    int best = expect - 10;
    for (int f = expect - 10; f <= expect + 10; f++)
      if (bp.onsetAt(f, c.second) > bp.onsetAt(best, c.second)) best = f;
    const float note = bp.noteAt(expect + 20, c.second);
    printf("  %d at %.1f s: onset peak frame %d (expected %d, %.2f), note posterior %.2f\n", c.second, c.first, best, expect,
           bp.onsetAt(best, c.second), note);
    assert(abs(best - expect) <= 2 && note > 0.3);
  }

  // the whole estimator on the same audio: each tone found once, at the right pitch and time
  t0 = std::chrono::steady_clock::now();
  const std::vector<NoteEstimate> est = estimateNotes(s, x, &bp);
  printf("estimate: %zu notes, %.0f ms\n", est.size(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  for (const NoteEstimate &e : est) printf("  %d %.2f-%.2f s strength %.2f\n", e.pitch, e.f0 / fr, e.f1 / fr, e.strength);
  for (auto c : {std::make_tuple(1.0, 2.0, 60), std::make_tuple(1.5, 2.5, 64), std::make_tuple(4.5, 5.5, 67), std::make_tuple(6.0, 6.8, 55)})
  {
    int found = 0;
    for (const NoteEstimate &e : est)
      if (e.pitch == std::get<2>(c) && fabs(e.f0 / fr - std::get<0>(c)) < 0.08 && fabs(e.f1 / fr - std::get<1>(c)) < 0.12) found++;
    assert(found == 1);
  }
  assert(est.size() <= 6); // at most a stray short note or two

  // speed on three minutes
  {
    std::vector<float> y((size_t)(180 * SR));
    for (int k = 0; k < 90; k++) tone(y, 2 * k, 2 * k + 1.5, 55 + (k * 7) % 20), tone(y, 2 * k + 0.5, 2 * k + 1.8, 62 + (k * 5) % 12);
    const Spectrogram sy = analyze(y);
    BasicPitch by;
    auto ta = std::chrono::steady_clock::now();
    assert(runBasicPitch(BASIC_PITCH_MODEL, y, sy.frames, by));
    auto tb = std::chrono::steady_clock::now();
    std::vector<float> r = nmfRoll(sy, lo, hi, true);
    auto tc = std::chrono::steady_clock::now();
    r = sacfRoll(y, sy.frames, lo, hi);
    auto td = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    printf("180 s: basic-pitch %.0f ms, adapted NMF %.0f ms, SACF %.0f ms\n", ms(ta, tb), ms(tb, tc), ms(tc, td));
  }
  printf("ok\n");
}
