#include "nsgt.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>

#include "fft.h"

// WDL's FFT tops out at 32768 points, so the signal is processed in overlapping blocks
// (sliCQ-style). Each block gets one forward FFT; every bin then multiplies its Gaussian
// window into the spectrum and runs a small inverse FFT of just the window's support,
// which yields that bin's magnitude decimated to the hop.
static const int BLOCK = 32768;

static int nextPow2(int n)
{
  int p = 1;
  while (p < n) p *= 2;
  return p;
}

double Spectrogram::binOfPitch(double midiPitch) const
{
  double f = 440.0 * pow(2.0, (midiPitch - 69) / 12.0);
  return p.binsPerOctave * log2(f / p.fmin);
}

// Parameters come from user settings: keep them in the range the indexing below assumes.
static NSGTParams sanitize(NSGTParams p)
{
  p.sampleRate = std::max(8000.0, std::min(96000.0, p.sampleRate));
  p.binsPerOctave = std::max(12, std::min(120, p.binsPerOctave));
  p.hop = std::max(64, std::min(BLOCK / 8, nextPow2(std::max(1, std::min(p.hop, BLOCK))))); // power of two dividing the block
  p.fmin = std::max(10.0, std::min(p.sampleRate * 0.25, p.fmin));
  p.fmax = std::max(p.fmin * 2, std::min(p.sampleRate * 0.45, p.fmax));
  p.gamma = std::max(0.0, std::min(1000.0, p.gamma));
  p.bassCrossover = std::max(p.fmin * 2, std::min(p.fmax, p.bassCrossover));
  return p;
}

static Spectrogram computeOne(const NSGTParams &p, long long numSamples,
                              const std::function<void(long long, int, float *)> &read,
                              const std::function<bool(double)> &progress)
{
  numSamples = std::max(0LL, std::min(numSamples, (long long)INT_MAX * 64)); // frames fit in an int
  static bool fftInit = (WDL_fft_init(), true);
  (void)fftInit;

  const int N = BLOCK, hop = p.hop;
  const double sr = p.sampleRate, freqRes = sr / N;
  const double fmax = p.fmax;
  const double Q = 1 / (pow(2.0, 1.0 / p.binsPerOctave) - 1);
  // A bin's time kernel is a Gaussian with sigmaT = 1/(2 pi sigmaF). Its +-3 sigma must fit in a
  // quarter block, which caps how small gamma can usefully get (~3 Hz at 22050 Hz).
  const double minSigmaF = 1 / (2 * M_PI * (N / 4.0 / sr / 3));

  Spectrogram s;
  s.p = p;
  s.bins = std::max(1, (int)ceil(p.binsPerOctave * log2(fmax / p.fmin)));
  s.frames = (int)(numSamples / hop) + 1;
  s.mag.assign((size_t)s.frames * s.bins, 0);

  struct Bin { int k0, k1, M, step; std::vector<float> w; };
  std::vector<Bin> bins(s.bins);
  double maxSigmaT = 0;
  for (int b = 0; b < s.bins; b++)
  {
    const double f = p.fmin * pow(2.0, (double)b / p.binsPerOctave);
    const double sigmaF = std::max((f / Q + p.gamma) / (2 * sqrt(2 * log(2.0))), minSigmaF);
    maxSigmaT = std::max(maxSigmaT, 1 / (2 * M_PI * sigmaF));
    const int r = std::max(1, (int)ceil(3 * sigmaF / freqRes)), c = (int)lround(f / freqRes);
    Bin &bn = bins[b];
    bn.k0 = std::max(1, c - r);
    bn.k1 = std::min(N / 2 - 1, c + r);
    bn.M = std::max(nextPow2(bn.k1 - bn.k0 + 1), N / hop);
    bn.step = bn.M * hop / N;
    for (int k = bn.k0; k <= bn.k1; k++)
    {
      double d = k * freqRes - f;
      bn.w.push_back((float)exp(-d * d / (2 * sigmaF * sigmaF)));
    }
  }

  // Frames closer than `margin` to a block edge would see circular wrap-around; only keep the middle.
  const int margin = std::min(N / 4, (int)ceil(3 * maxSigmaT * sr / hop) * hop);
  const int valid = N - 2 * margin, framesPerBlock = valid / hop;

  std::vector<WDL_FFT_REAL> block(N);
  int maxM = 0;
  for (const Bin &bn : bins) maxM = std::max(maxM, bn.M);
  std::vector<WDL_FFT_COMPLEX> small(maxM);
  const int *permN = WDL_fft_permute_tab(N / 2);
  // Y = 2*X*w for the analytic signal and |ifft|/N; WDL's real FFT output is already 2x a plain DFT, so 2/N -> 1/N.
  const float scale = 1.0f / N;

  for (int first = 0; first < s.frames; first += framesPerBlock)
  {
    const long long start = (long long)first * hop - margin;
    read(start, N, block.data());
    WDL_real_fft(block.data(), N, 0);
    const WDL_FFT_COMPLEX *X = (const WDL_FFT_COMPLEX *)block.data();
    const int nf = std::min(framesPerBlock, s.frames - first);

    for (int b = 0; b < s.bins; b++)
    {
      const Bin &bn = bins[b];
      const int *permM = WDL_fft_permute_tab(bn.M);
      memset(small.data(), 0, bn.M * sizeof(WDL_FFT_COMPLEX));
      // Shifting the band down to bin 0 only changes phase, not magnitude.
      for (int k = bn.k0; k <= bn.k1; k++)
      {
        const WDL_FFT_COMPLEX &x = X[permN[k]];
        WDL_FFT_COMPLEX &y = small[permM[k - bn.k0]];
        float w = bn.w[k - bn.k0] * scale;
        y.re = x.re * w;
        y.im = x.im * w;
      }
      WDL_fft(small.data(), bn.M, 1);
      for (int i = 0; i < nf; i++)
      {
        const WDL_FFT_COMPLEX &z = small[(margin / hop + i) * bn.step];
        s.mag[(size_t)(first + i) * s.bins + b] = hypotf(z.re, z.im);
      }
    }
    if (progress && !progress((double)(first + nf) / s.frames)) return s;
  }

  return s;
}

Spectrogram computeNSGT(const NSGTParams &params, long long numSamples,
                        const std::function<void(long long, int, float *)> &read,
                        const std::function<bool(double)> &progress)
{
  const NSGTParams p = sanitize(params);
  if (!p.bass)
  {
    Spectrogram s = computeOne(p, numSamples, read, progress);
    estimateNoise(s, 0.1);
    return s;
  }
  // the bass pass only covers up to the crossover, so it is much cheaper than the main one
  const double mainShare = 0.8;
  Spectrogram s = computeOne(p, numSamples, read, [&](double f) { return !progress || progress(f * mainShare); });
  NSGTParams bp = p;
  bp.gamma = 0, bp.fmax = std::max(p.fmin * 2, std::min(p.fmax, p.bassCrossover * 1.01));
  const Spectrogram bass = computeOne(bp, numSamples, read, [&](double f) { return !progress || progress(mainShare + f * (1 - mainShare)); });
  // same fmin, bins per octave and hop, so bins and frames line up
  const int nb = std::min(s.bins, bass.bins), nf = std::min(s.frames, bass.frames);
  for (int b = 0; b < nb; b++)
  {
    const double f = p.fmin * pow(2.0, (double)b / p.binsPerOctave);
    const double w = std::max(0.0, std::min(1.0, log2(p.bassCrossover / f) / 0.5)); // 1 half an octave below
    if (w <= 0) continue;
    for (int fr = 0; fr < nf; fr++)
    {
      float &m = s.mag[(size_t)fr * s.bins + b];
      m = (float)(m * (1 - w) + bass.at(fr, b) * w);
    }
  }
  estimateNoise(s, 0.1);
  return s;
}

void estimateNoise(Spectrogram &s, double fraction)
{
  std::vector<std::pair<double, int>> energy(s.frames);
  for (int f = 0; f < s.frames; f++)
  {
    double e = 0;
    for (int b = 0; b < s.bins; b++) e += (double)s.at(f, b) * s.at(f, b);
    energy[f] = {e, f};
  }
  s.noise.assign(s.bins, 0);
  if (!s.frames) return;
  const int quiet = std::max(1, std::min(s.frames, (int)(s.frames * fraction)));
  std::nth_element(energy.begin(), energy.begin() + quiet - 1, energy.end());
  for (int i = 0; i < quiet; i++)
    for (int b = 0; b < s.bins; b++) s.noise[b] += s.at(energy[i].second, b) / quiet;
}

void autoCalibrate(const Spectrogram &s, RenderParams &rp, int f0, int f1)
{
  if (f1 < 0 || f1 > s.frames) f1 = s.frames;
  f0 = std::max(0, std::min(f0, f1));
  std::vector<float> db;
  const size_t a = (size_t)f0 * s.bins, n = (size_t)f1 * s.bins, stride = std::max<size_t>(1, (n - a) / 200000);
  for (size_t i = a; i < n; i += stride) db.push_back(20 * log10f(std::max(s.mag[i], 1e-10f)));
  if (db.empty()) return;
  auto pct = [&](double q) {
    auto it = db.begin() + std::min(db.size() - 1, (size_t)(q * db.size()));
    std::nth_element(db.begin(), it, db.end());
    return (double)*it;
  };
  const double p30 = pct(0.30), p95 = pct(0.95);
  rp.dbFloor = std::max(std::min(p30, -20.0), -120.0);
  const double np95 = (p95 - rp.dbFloor) / -rp.dbFloor;
  rp.contrast = np95 > 0.01 ? std::max(0.1, std::min(3.0, 0.95 / np95)) : 1;
  rp.brightness = -0.10;
}

static float displayMag(const Spectrogram &s, const RenderParams &rp, int f, int b)
{
  float m = s.at(f, b);
  return rp.subtractNoise ? std::max(0.0f, m - (float)rp.noiseAlpha * s.noise[b]) : m;
}

double cellValue(const Spectrogram &s, const RenderParams &rp, int f, int b)
{
  const float m = displayMag(s, rp, f, b);
  const double db = std::max(20.0 * log10(std::max(m, 1e-10f)), rp.dbFloor);
  double v = ((db - rp.dbFloor) / -rp.dbFloor + rp.brightness) * rp.contrast;
  v = std::max(0.0, std::min(1.0, v));
  if (rp.peaksOnly && b > 0 && b + 1 < s.bins &&
      !(m > displayMag(s, rp, f, b - 1) && m > displayMag(s, rp, f, b + 1)))
    v *= rp.peakDim;
  return v;
}

// Maps a float's exponent and top 8 mantissa bits (~0.03 dB steps) straight to the display value.
static const int LUT_SHIFT = 15;

void renderValues(const Spectrogram &s, const RenderParams &rp, std::vector<unsigned char> &out)
{
  static std::vector<unsigned char> lut(1 << (32 - LUT_SHIFT - 1));
  for (size_t i = 0; i < lut.size(); i++)
  {
    unsigned int bits = (unsigned int)(i << LUT_SHIFT) | (1u << (LUT_SHIFT - 1)); // bucket centre
    float m;
    memcpy(&m, &bits, 4);
    const double db = std::max(20.0 * log10(std::max(m, 1e-10f)), rp.dbFloor);
    double v = ((db - rp.dbFloor) / -rp.dbFloor + rp.brightness) * rp.contrast;
    lut[i] = (unsigned char)lround(std::max(0.0, std::min(1.0, v)) * 255);
  }
  auto value = [&](float m) {
    unsigned int bits;
    memcpy(&bits, &m, 4);
    return lut[(bits & 0x7fffffff) >> LUT_SHIFT];
  };

  out.resize(s.mag.size());
  std::vector<float> row(s.bins);
  const float alpha = (float)rp.noiseAlpha;
  const int dim = (int)lround(std::max(0.0, std::min(1.0, rp.peakDim)) * 256);
  for (int f = 0; f < s.frames; f++)
  {
    const float *m = &s.mag[(size_t)f * s.bins];
    if (rp.subtractNoise)
      for (int b = 0; b < s.bins; b++) row[b] = std::max(0.0f, m[b] - alpha * s.noise[b]);
    else
      memcpy(row.data(), m, s.bins * sizeof(float));
    unsigned char *o = &out[(size_t)f * s.bins];
    for (int b = 0; b < s.bins; b++)
    {
      int v = value(row[b]);
      if (rp.peaksOnly && b > 0 && b + 1 < s.bins && !(row[b] > row[b - 1] && row[b] > row[b + 1])) v = (v * dim) >> 8;
      o[b] = (unsigned char)v;
    }
  }
}

void makeColormap(double bias, unsigned int lut[256])
{
  static const float magma[9][3] = {
      {0.001f, 0.000f, 0.014f}, {0.090f, 0.048f, 0.228f}, {0.232f, 0.060f, 0.438f},
      {0.390f, 0.100f, 0.502f}, {0.550f, 0.161f, 0.506f}, {0.716f, 0.214f, 0.475f},
      {0.868f, 0.340f, 0.443f}, {0.967f, 0.534f, 0.498f}, {0.987f, 0.991f, 0.750f},
  };
  const double exponent = pow(2.0, -bias);
  for (int i = 0; i < 256; i++)
  {
    const double t = pow(i / 255.0, exponent) * 8;
    const int k = std::min(7, (int)t);
    const double fr = t - k;
    unsigned int c = 0;
    for (int ch = 0; ch < 3; ch++)
    {
      double v = magma[k][ch] + (magma[k + 1][ch] - magma[k][ch]) * fr;
      c = (c << 8) | (unsigned int)std::max(0.0, std::min(255.0, v * 255));
    }
    lut[i] = c;
  }
}
