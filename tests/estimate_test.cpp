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
static void tone(std::vector<float> &x, double a, double b, double midi, float amp = 0.1f, double rate = 5.5,
                 double depth = 0.004, double evenGain = 1, double vibPhase = 0)
{
  srand(1);
  const double f0 = 440 * pow(2, (midi - 69) / 12);
  double phase[9] = {};
  for (int h = 1; h <= 8; h++) phase[h] = rand() / (double)RAND_MAX * 6.3;
  double ph = 0;
  for (long long i = (long long)(a * SR); i < (long long)(b * SR) && i < (long long)x.size(); i++)
  {
    const double t = i / SR, env = std::min(1.0, std::min(t - a, b - t) / 0.02);
    ph += 2 * M_PI * f0 * (1 + depth * sin(2 * M_PI * rate * t + vibPhase)) / SR;
    double v = 0;
    for (int h = 1; h <= 8; h++) v += sin(h * ph + phase[h]) * pow(h, -0.6) * (h % 2 ? 1 : evenGain);
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

  // Voices: a held top line over a moving lower one stay in their strands; the lead is the busier strand
  {
    const double fr = 10;
    const std::vector<NoteEstimate> v1 = {{0, 40, 69, 1}, {0, 20, 64, 1}, {20, 40, 62, 1}, {40, 60, 69, 1}, {40, 60, 65, 1}, {60, 70, 66, 1}};
    const std::vector<int> a = assignVoices(v1, fr, 4);
    printf("voices: %d %d %d %d %d %d\n", a[0], a[1], a[2], a[3], a[4], a[5]);
    assert(a[0] == a[3] && a[1] == a[2] && a[2] == a[4] && a[4] == a[5] && a[0] != a[1] && a[0] >= 0 && a[1] >= 0);
    assert(a[1] == 0); // the lower strand sings 70 frames, the upper 60
    // a stepwise solo line after a two-note chord stays in one strand
    std::vector<NoteEstimate> v2 = {{0, 10, 72, 1}, {0, 10, 65, 1}};
    for (int k = 0; k < 4; k++) v2.push_back({100 + 10 * k, 110 + 10 * k, 67 - k, 1});
    const std::vector<int> b = assignVoices(v2, fr, 4);
    assert(b[2] == b[3] && b[3] == b[4] && b[4] == b[5] && b[2] == 0);
    // more notes than voices: the weakest is crowded out; one voice only for a solo line
    const std::vector<int> c = assignVoices({{0, 10, 60, 1}, {0, 10, 64, 0.5f}, {0, 10, 67, 0.9f}}, fr, 2);
    assert(c[1] == -1 && c[0] >= 0 && c[2] >= 0 && c[0] != c[2]);
    const std::vector<int> d = assignVoices({{0, 10, 60, 1}, {10, 20, 62, 1}, {20, 30, 64, 1}}, fr, 4);
    assert(d[0] == 0 && d[1] == 0 && d[2] == 0);
  }

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

  // octave doublings: a second singer an octave up (own vibrato) is added; one voice with strong even
  // partials (a formant) is not, since its "octave" moves with its own partials
  {
    std::vector<float> y((size_t)(3 * SR));
    for (float &v : y) v = 0.0005f * ((float)(rand() / (double)RAND_MAX) - 0.5f);
    tone(y, 0.5, 2.5, 62, 0.1f, 5.5, 0.006), tone(y, 0.5, 2.5, 74, 0.06f, 4.3, 0.006, 1, 2.0);
    const Spectrogram sy = analyze(y);
    BasicPitch by;
    assert(runBasicPitch(BASIC_PITCH_MODEL, y, sy.frames, by));
    std::vector<NoteEstimate> e = estimateNotes(sy, y, &by);
    EstimateParams off;
    off.octaveDoublings = false;
    const std::vector<NoteEstimate> e0 = estimateNotes(sy, y, &by, off);
    auto has = [](const std::vector<NoteEstimate> &v, int p) {
      for (const NoteEstimate &n : v) if (n.pitch == p) return true;
      return false;
    };
    printf("octave pair: D5 found %s without doublings, %s with\n", has(e0, 74) ? "yes" : "no", has(e, 74) ? "yes" : "no");
    assert(has(e, 62) && has(e, 74));

    std::vector<float> z((size_t)(3 * SR));
    for (float &v : z) v = 0.0005f * ((float)(rand() / (double)RAND_MAX) - 0.5f);
    tone(z, 0.5, 2.5, 62, 0.1f, 5.5, 0.006, 3.0);
    const Spectrogram sz = analyze(z);
    BasicPitch bz;
    assert(runBasicPitch(BASIC_PITCH_MODEL, z, sz.frames, bz));
    e = estimateNotes(sz, z, &bz);
    for (const NoteEstimate &n : e) printf("  formant tone: %d %.2f-%.2f s\n", n.pitch, n.f0 / fr, n.f1 / fr);
    assert(has(e, 62) && !has(e, 74));
  }

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
    const std::vector<NoteEstimate> all = estimateNotes(sy, y, &by);
    auto te = std::chrono::steady_clock::now();
    const std::vector<int> voices = assignVoices(all, sy.frameRate(), 4);
    auto tf = std::chrono::steady_clock::now();
    printf("180 s: whole estimator %.0f ms (%zu notes), voices %.0f ms\n", ms(td, te), all.size(), ms(te, tf));
  }
  printf("ok\n");
}
