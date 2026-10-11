#include "estimate.h"

#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <thread>

#include "fft.h"

static const int SR = 22050, HOP = 256;

NSGTParams estimateNSGTParams() { return NSGTParams(); } // 60 bins per octave, gamma 11 Hz: as tested in transcribe

// Divide by the 99th percentile (numpy's linear interpolation) of the frame maxima.
static void normaliseP99(std::vector<float> &roll, int n)
{
  const int F = n > 0 ? (int)(roll.size() / n) : 0;
  if (!F) return;
  std::vector<float> mx(F);
  for (int f = 0; f < F; f++) mx[f] = *std::max_element(roll.begin() + (size_t)f * n, roll.begin() + (size_t)(f + 1) * n);
  std::sort(mx.begin(), mx.end());
  const double pos = 0.99 * (F - 1);
  const int i = (int)pos;
  const double v = std::max(1e-12, mx[i] + (i + 1 < F ? (mx[i + 1] - mx[i]) * (pos - i) : 0.0));
  for (float &x : roll) x = (float)(x / v);
}

// --- harmonic-template NMF (transcribe mf0.templates / nmf / adapt) ------------------------------

static const int SUB = 3;                // atoms per semitone
static const int NH = 12;                // partials per comb
static const double ROLLOFF = 0.6;       // partial h has amplitude h^-ROLLOFF
static const double WIDTH = 0.6;         // partial width in bins at 36 bins per octave
static const double LAM = 0.02;          // L1 penalty relative to frame energy
static const int ITERS = 150, OUTER = 3, INNER = 25;
static const int REGION = 6;             // semitones sharing adapted partial amplitudes
static const double CLAMP = 1.25, SOLO = 0.6;
static const int CHUNK = 1024;           // frames per matrix product, to bound memory

std::vector<float> nmfRoll(const Spectrogram &s, int lo, int hi, bool adapt, const std::function<bool(double)> &progress)
{
  const int n = hi - lo + 1, F = s.frames, bpo = s.p.binsPerOctave;
  std::vector<float> roll((size_t)std::max(0, F) * std::max(0, n), 0);
  const int b0 = std::max(0, (int)lround(s.binOfPitch(36))), B = s.bins - b0; // the axis starts at C2, as in transcribe
  if (F <= 0 || B <= 0 || n <= 0) return roll;
  const int pmin = std::min(45, lo - 10), pmax = std::max(86, hi + 2), P = (pmax - pmin) * SUB + 1;
  const double width = WIDTH * bpo / 36.0;
  auto pitchOf = [&](int p) { return pmin + (double)p / SUB; };

  // Gaussian partials: part[(b * P + p) * NH + h - 1]
  std::vector<float> part((size_t)B * P * NH, 0);
  for (int p = 0; p < P; p++)
    for (int h = 1; h <= NH; h++)
    {
      const double c = s.binOfPitch(pitchOf(p) + 12 * log2((double)h)) - b0;
      if (c >= B) break;
      for (int b = std::max(0, (int)floor(c - 5 * width)); b <= std::min(B - 1, (int)ceil(c + 5 * width)); b++)
        part[((size_t)b * P + p) * NH + h - 1] = (float)exp(-0.5 * ((b - c) / width) * ((b - c) / width));
    }
  // broadband atoms (half-octave bumps) soak up breath and consonants so low combs don't
  std::vector<std::vector<float>> noise;
  for (int c = 0; c < B; c += bpo / 2)
  {
    std::vector<float> col(B);
    double sum = 0;
    for (int b = 0; b < B; b++) sum += col[b] = (float)exp(-0.5 * ((b - c) / (bpo / 3.0)) * ((b - c) / (bpo / 3.0)));
    for (float &v : col) v = (float)(v / sum);
    noise.push_back(col);
  }
  const int K = P + (int)noise.size();
  const int regions = (P - 1) / (SUB * REGION) + 1;
  auto reg = [](int p) { return p / (SUB * REGION); };
  std::vector<double> init(NH);
  for (int h = 0; h < NH; h++) init[h] = pow(h + 1.0, -ROLLOFF);
  std::vector<double> A((size_t)regions * NH);
  for (int r = 0; r < regions; r++) std::copy(init.begin(), init.end(), A.begin() + (size_t)r * NH);

  auto V = [&](int f, int b) { return s.mag[(size_t)f * s.bins + b0 + b]; };
  const float eps = 1e-9f;
  double mean = 0;
  std::vector<float> pen(F);
  for (int f = 0; f < F; f++)
  {
    double sum = 0;
    for (int b = 0; b < B; b++) sum += V(f, b);
    mean += sum, pen[f] = (float)(LAM * (sum + eps) / B * 50);
  }
  mean /= (double)F * B;

  std::vector<float> H((size_t)F * K, (float)(mean + eps)), W((size_t)B * K), norm(P);
  std::vector<float> WH((size_t)CHUNK * B), NUM((size_t)CHUNK * K);
  auto buildW = [&] {
    for (int p = 0; p < P; p++)
    {
      double sum = 0;
      for (int b = 0; b < B; b++)
      {
        double v = 0;
        const float *pp = &part[((size_t)b * P + p) * NH];
        for (int h = 0; h < NH; h++) v += pp[h] * A[(size_t)reg(p) * NH + h];
        W[(size_t)b * K + p] = (float)v, sum += v;
      }
      norm[p] = (float)(sum + eps);
      for (int b = 0; b < B; b++) W[(size_t)b * K + p] /= norm[p];
    }
    for (size_t j = 0; j < noise.size(); j++)
      for (int b = 0; b < B; b++) W[(size_t)b * K + P + j] = noise[j][b];
  };
  // WH = V / (H W^T) for frames c0..c0+fc
  auto ratio = [&](int c0, int fc) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, fc, B, K, 1.0f, &H[(size_t)c0 * K], K, W.data(), K, 0.0f, WH.data(), B);
    for (int f = 0; f < fc; f++)
      for (int b = 0; b < B; b++) WH[(size_t)f * B + b] = V(c0 + f, b) / (WH[(size_t)f * B + b] + eps);
  };
  int done = 0;
  // KL multiplicative updates with W fixed; W's columns sum to 1
  auto iterate = [&](int count) {
    for (int it = 0; it < count; it++, done++)
    {
      for (int c0 = 0; c0 < F; c0 += CHUNK)
      {
        const int fc = std::min(CHUNK, F - c0);
        ratio(c0, fc);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, fc, K, B, 1.0f, WH.data(), B, W.data(), K, 0.0f, NUM.data(), K);
        for (int f = 0; f < fc; f++)
        {
          float *h = &H[(size_t)(c0 + f) * K];
          const float *num = &NUM[(size_t)f * K], d = 1 + pen[c0 + f];
          for (int k = 0; k < K; k++) h[k] *= num[k] / d;
        }
      }
      if (progress && !progress((double)done / ITERS)) return false;
    }
    return true;
  };
  // Re-estimates the partial amplitudes per region from solo-ish frames (Vincent, Bertin & Badeau 2010).
  auto adaptStep = [&] {
    std::vector<float> RH((size_t)B * P, 0), Hm((size_t)CHUNK * P);
    std::vector<double> hsum(P, 0);
    for (int c0 = 0; c0 < F; c0 += CHUNK)
    {
      const int fc = std::min(CHUNK, F - c0);
      ratio(c0, fc);
      for (int f = 0; f < fc; f++)
      {
        const float *h = &H[(size_t)(c0 + f) * K];
        double sum = 0, best = 0;
        for (int p = 0; p < P; p++)
        {
          sum += h[p];
          best = std::max(best, (double)(p > 0 ? h[p - 1] : 0) + h[p] + (p + 1 < P ? h[p + 1] : 0));
        }
        const bool solo = best / (sum + eps) >= SOLO;
        for (int p = 0; p < P; p++) Hm[(size_t)f * P + p] = solo ? h[p] : 0, hsum[p] += solo ? h[p] : 0;
      }
      cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans, B, P, fc, 1.0f, WH.data(), B, Hm.data(), P, 1.0f, RH.data(), P);
    }
    std::vector<double> nr((size_t)regions * NH, 0), dr((size_t)regions * NH, 0);
    for (int p = 0; p < P; p++)
      for (int b = 0; b < B; b++)
      {
        const float *pp = &part[((size_t)b * P + p) * NH];
        const double rh = RH[(size_t)b * P + p] / norm[p], hn = hsum[p] / norm[p];
        for (int h = 0; h < NH; h++) nr[(size_t)reg(p) * NH + h] += pp[h] * rh, dr[(size_t)reg(p) * NH + h] += pp[h] * hn;
      }
    for (size_t i = 0; i < A.size(); i++) A[i] *= (nr[i] + eps) / (dr[i] + eps);
    for (int r = 0; r < regions; r++)
    {
      const double a0 = std::max((double)eps, A[(size_t)r * NH]);
      for (int h = 0; h < NH; h++) A[(size_t)r * NH + h] /= a0;
    }
    if (regions > 2) // smooth across regions in the log domain
    {
      std::vector<double> L(A.size());
      for (size_t i = 0; i < A.size(); i++) L[i] = log(std::max((double)eps, A[i]));
      for (int r = 1; r + 1 < regions; r++)
        for (int h = 0; h < NH; h++)
          A[(size_t)r * NH + h] = exp((L[(size_t)(r - 1) * NH + h] + 2 * L[(size_t)r * NH + h] + L[(size_t)(r + 1) * NH + h]) / 4);
    }
    for (int r = 0; r < regions; r++)
      for (int h = 0; h < NH; h++)
      {
        double &a = A[(size_t)r * NH + h];
        a = std::max(init[h] / CLAMP, std::min(init[h] * CLAMP, a));
      }
  };

  const int outer = adapt ? OUTER : 0;
  for (int it = 0;; it++)
  {
    buildW();
    if (!iterate(it < outer ? INNER : ITERS - outer * INNER)) return roll;
    if (it == outer) break;
    adaptStep();
  }

  // pool the three atoms of each semitone
  for (int f = 0; f < F; f++)
    for (int m = lo; m <= hi; m++)
    {
      double sum = 0;
      for (int j = SUB * (m - pmin) - 1; j <= SUB * (m - pmin) + 1; j++)
        if (j >= 0 && j < P) sum += H[(size_t)f * K + j];
      roll[(size_t)f * n + m - lo] = (float)sum;
    }
  normaliseP99(roll, n);
  return roll;
}

// --- enhanced summary autocorrelation (transcribe bench/approaches/a04_sacf.py) ------------------

static const int SACF_BANDS = 16;
static const double SACF_LOW = 80, SACF_HIGH = 6000; // band edges, Hz
static const double SACF_K = 0.3;                     // generalised ACF: ifft(|X|^K)
static const int SACF_NFFT = 2048, SACF_UP = 4;       // 93 ms frames; lag oversampling
static const int SACF_MARGIN = SR / 2;                // filter run-in around each chunk, samples

struct Biquad { double b0, b1, b2, a1, a2; };

// scipy.signal.butter(2, [lo, hi], 'band'): the analog prototype moved to the band and through the
// bilinear transform, as two second-order sections.
static void butterBandpass(double lo, double hi, Biquad out[2])
{
  typedef std::complex<double> C;
  const double fs = 2, w1 = 2 * fs * tan(M_PI * (lo / (SR / 2.0)) / fs), w2 = 2 * fs * tan(M_PI * (hi / (SR / 2.0)) / fs);
  const double bw = w2 - w1, wo = sqrt(w1 * w2), fs2 = 2 * fs;
  const C proto[2] = {-std::exp(C(0, -M_PI / 4)), -std::exp(C(0, M_PI / 4))};
  C poles[4];
  for (int i = 0; i < 2; i++)
  {
    const C pl = proto[i] * (bw / 2), r = std::sqrt(pl * pl - wo * wo);
    poles[i] = pl + r, poles[2 + i] = pl - r;
  }
  C den = 1;
  for (C &p : poles) den *= fs2 - p, p = (fs2 + p) / (fs2 - p);
  const double k = bw * bw * std::real(C(fs2 * fs2) / den); // zeros: two at s=0 (z=1), two at infinity (z=-1)
  for (int s = 0; s < 2; s++)
  {
    const C a = poles[2 * s], b = poles[2 * s + 1];
    const double g = s == 0 ? k : 1;
    out[s] = {g, 0, -g, -std::real(a + b), std::real(a * b)};
  }
}

static void biquadRun(const Biquad &q, float *x, int n, int step)
{
  double z1 = 0, z2 = 0;
  for (int i = 0; i < n; i++)
  {
    float &v = x[(size_t)i * step];
    const double in = v, y = q.b0 * in + z1;
    z1 = q.b1 * in - q.a1 * y + z2;
    z2 = q.b2 * in - q.a2 * y;
    v = (float)y;
  }
}

// zero-phase: forward, then backward
static void filtfilt(const Biquad sos[2], std::vector<float> &x)
{
  const int n = (int)x.size();
  if (!n) return;
  for (int s = 0; s < 2; s++) biquadRun(sos[s], x.data(), n, 1);
  for (int s = 0; s < 2; s++) biquadRun(sos[s], x.data() + n - 1, n, -1);
}

std::vector<float> sacfRoll(const std::vector<float> &mono, int frames, int lo, int hi, const std::function<bool(double)> &progress)
{
  const int n = hi - lo + 1, F = std::max(0, frames);
  std::vector<float> roll((size_t)F * std::max(0, n), 0);
  if (!F || n <= 0) return roll;
  static bool fftInit = (WDL_fft_init(), true);
  (void)fftInit;

  Biquad bands[SACF_BANDS][2];
  for (int i = 0; i < SACF_BANDS; i++)
    butterBandpass(SACF_LOW * pow(SACF_HIGH / SACF_LOW, (double)i / SACF_BANDS),
                   SACF_LOW * pow(SACF_HIGH / SACF_LOW, (double)(i + 1) / SACF_BANDS), bands[i]);
  const int maxlag = (int)(SR / 70.0 * SACF_UP), NI = SACF_NFFT * SACF_UP;
  // lags of each semitone: within +-50 cents, else the nearest lag
  std::vector<int> la(n), lb(n);
  for (int k = lo; k <= hi; k++)
  {
    int a = maxlag, b = 0, nearest = 1;
    for (int l = 1; l < maxlag; l++)
    {
      const double m = 69 + 12 * log2((double)SR * SACF_UP / l / 440);
      if (fabs(m - k) <= 0.5) a = std::min(a, l), b = std::max(b, l);
      if (fabs(m - k) < fabs(69 + 12 * log2((double)SR * SACF_UP / nearest / 440) - k)) nearest = l;
    }
    if (a > b) a = b = nearest;
    la[k - lo] = a, lb[k - lo] = b;
  }
  std::vector<float> hann(SACF_NFFT);
  for (int i = 0; i < SACF_NFFT; i++) hann[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / SACF_NFFT));

  const int chunk = 512, chunks = (F + chunk - 1) / chunk;
  std::atomic<int> next{0}, finished{0};
  std::atomic<bool> cancel{false};
  auto worker = [&] {
    std::vector<float> x, acc((size_t)chunk * maxlag), st(maxlag);
    std::vector<WDL_FFT_REAL> buf(SACF_NFFT), ibuf(NI);
    const int *perm = WDL_fft_permute_tab(SACF_NFFT / 2), *iperm = WDL_fft_permute_tab(NI / 2);
    std::vector<float> pw(SACF_NFFT / 2 + 1), expo(pw.size(), (float)(SACF_K / 2));
    int bins = (int)pw.size();
    for (int ci; !cancel && (ci = next++) < chunks;)
    {
      const int c0 = ci * chunk, fc = std::min(chunk, F - c0);
      const long long s0 = (long long)c0 * HOP - SACF_NFFT / 2 - SACF_MARGIN;
      const int len = (fc - 1) * HOP + SACF_NFFT + 2 * SACF_MARGIN;
      std::fill(acc.begin(), acc.end(), 0.0f);
      for (int band = 0; band < SACF_BANDS; band++)
      {
        x.assign(len, 0);
        for (int i = 0; i < len; i++)
        {
          const long long j = s0 + i;
          if (j >= 0 && j < (long long)mono.size()) x[i] = mono[j];
        }
        filtfilt(bands[band], x);
        for (int f = 0; f < fc; f++)
        {
          const float *seg = &x[(size_t)f * HOP + SACF_MARGIN]; // frame centred on (c0+f)*HOP
          for (int i = 0; i < SACF_NFFT; i++) buf[i] = seg[i] * hann[i];
          WDL_real_fft(buf.data(), SACF_NFFT, 0);
          const WDL_FFT_COMPLEX *X = (const WDL_FFT_COMPLEX *)buf.data();
          // |X|^K = (|X|^2)^(K/2), vectorised
          pw[0] = X[0].re * X[0].re, pw[SACF_NFFT / 2] = X[0].im * X[0].im; // Nyquist is packed into bin 0
          for (int k = 1; k < SACF_NFFT / 2; k++) pw[k] = X[perm[k]].re * X[perm[k]].re + X[perm[k]].im * X[perm[k]].im;
          vvpowf(pw.data(), expo.data(), pw.data(), &bins);
          std::fill(ibuf.begin(), ibuf.end(), 0);
          WDL_FFT_COMPLEX *Y = (WDL_FFT_COMPLEX *)ibuf.data();
          for (int k = 0; k <= SACF_NFFT / 2; k++) Y[iperm[k]].re = pw[k];
          WDL_real_fft(ibuf.data(), NI, 1);
          float *a = &acc[(size_t)f * maxlag];
          for (int l = 0; l < maxlag; l++) a[l] += (float)ibuf[l];
        }
      }
      for (int f = 0; f < fc; f++)
      {
        float *S = &acc[(size_t)f * maxlag];
        for (int l = 0; l < maxlag; l++) S[l] = std::max(0.0f, S[l]);
        // "enhanced": remove the peaks at multiples of each period (sub-octaves)
        for (int fct = 2; fct <= 3; fct++)
        {
          for (int l = 0; l < maxlag; l++)
          {
            const double pos = (double)l / fct;
            const int i = (int)pos;
            st[l] = S[i] + (i + 1 < maxlag ? (S[i + 1] - S[i]) * (float)(pos - i) : 0);
          }
          for (int l = 0; l < maxlag; l++) S[l] = std::max(0.0f, S[l] - st[l]);
        }
        for (int k = 0; k < n; k++)
        {
          float m = 0;
          for (int l = la[k]; l <= lb[k]; l++) m = std::max(m, S[l]);
          roll[(size_t)(c0 + f) * n + k] = m;
        }
      }
      const int d = ++finished;
      (void)d;
    }
  };
  const int threads = std::max(1, std::min(chunks, (int)std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; t++) pool.emplace_back(worker);
  // the calling thread works too, and reports progress between its chunks
  {
    std::thread self(worker);
    while (finished < chunks && !cancel)
    {
      if (progress && !progress((double)finished / chunks)) cancel = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    self.join();
  }
  for (std::thread &t : pool) t.join();
  if (cancel) return roll;
  normaliseP99(roll, n);
  return roll;
}

// --- activity and segmentation (transcribe notes.frame_active; hysteresis instead of a grid) -----

struct Activity
{
  std::vector<float> S;                  // median filtered and gated
  std::vector<unsigned char> on, hold;   // starts a note / keeps one going
};

// Median over 5 frames along time ("reflect" edges as scipy), gate, local pitch peaks, absolute and
// relative thresholds (halved for `hold`), and no upper octave weaker than OCT x the note below.
static Activity activity(const std::vector<float> &roll, int F, int n, const std::vector<unsigned char> *gate,
                         double absT, double rel, double oct)
{
  Activity a;
  a.S.assign(roll.size(), 0), a.on.assign(roll.size(), 0), a.hold.assign(roll.size(), 0);
  auto refl = [F](int i) { return i < 0 ? -i - 1 : i >= F ? 2 * F - i - 1 : i; };
  float w[5];
  for (int f = 0; f < F; f++)
    for (int p = 0; p < n; p++)
    {
      for (int d = -2; d <= 2; d++) w[d + 2] = roll[(size_t)refl(std::max(-F, std::min(2 * F - 1, f + d))) * n + p];
      std::nth_element(w, w + 2, w + 5);
      a.S[(size_t)f * n + p] = gate && !(*gate)[(size_t)f * n + p] ? 0 : w[2];
    }
  for (int f = 0; f < F; f++)
  {
    const float *S = &a.S[(size_t)f * n];
    const float mx = *std::max_element(S, S + n);
    for (int level = 0; level < 2; level++)
    {
      const double k = level ? 0.5 : 1;
      unsigned char *act = level ? &a.hold[(size_t)f * n] : &a.on[(size_t)f * n];
      for (int p = 0; p < n; p++)
      {
        const bool peak = (p == 0 || S[p] >= S[p - 1]) && (p + 1 == n || S[p] >= S[p + 1]);
        act[p] = peak && S[p] > absT * k && S[p] > rel * k * mx;
      }
      for (int p = n - 1; p >= 12; p--)
        if (act[p] && act[p - 12] && S[p] < oct * S[p - 12]) act[p] = 0; // the lower note's 2nd partial
    }
  }
  return a;
}

std::vector<NoteEstimate> estimateNotes(const Spectrogram &s, const std::vector<float> &mono, const BasicPitch *bp,
                                        const EstimateParams &ep, const std::function<bool(double)> &progress)
{
  std::vector<NoteEstimate> out;
  const int lo = ep.lo, hi = ep.hi, n = hi - lo + 1, F = s.frames;
  if (F <= 0 || n <= 0) return out;
  const double nmfShare = ep.sacfAbs > 0 ? 0.6 : 1;
  bool cancelled = false;
  auto stage = [&](double a, double b) {
    return [&, a, b](double f) { return !(cancelled = progress && !progress(a + (b - a) * f)); };
  };
  std::vector<float> roll = nmfRoll(s, lo, hi, ep.adapt, stage(0, nmfShare));
  if (cancelled) return out;

  std::vector<unsigned char> gate;
  if (bp && bp->frames > 0)
  {
    gate.assign((size_t)F * n, 0);
    for (int f = 0; f < F; f++)
    {
      const int g = std::min(f, bp->frames - 1);
      for (int p = 0; p < n; p++)
      {
        float m = 0;
        for (int d = -1; d <= 1; d++)
        {
          const int midi = lo + p + d;
          if (midi >= BasicPitch::LOWEST && midi < BasicPitch::LOWEST + BasicPitch::PITCHES) m = std::max(m, bp->noteAt(g, midi));
        }
        gate[(size_t)f * n + p] = m > ep.gate;
      }
    }
  }
  const std::vector<unsigned char> *gp = gate.empty() ? nullptr : &gate;
  Activity act = activity(roll, F, n, gp, ep.abs, ep.rel, ep.oct);
  if (ep.sacfAbs > 0)
  {
    std::vector<float> sroll = sacfRoll(mono, F, lo, hi, stage(nmfShare, 1));
    if (cancelled) return out;
    // the SACF fails on different notes than the NMF: add what it finds confidently
    const Activity sa = activity(sroll, F, n, gp, ep.sacfAbs, ep.rel, ep.oct);
    for (size_t i = 0; i < act.on.size(); i++)
    {
      act.on[i] |= sa.on[i], act.hold[i] |= sa.hold[i];
      act.S[i] = std::max(act.S[i], sa.S[i] * (float)(ep.abs / ep.sacfAbs)); // on the NMF's scale
    }
  }

  // runs: start on an `on` frame, continue over `hold` frames bridging short dropouts, split where
  // basic-pitch sees a new onset at that pitch
  const double fr = (double)SR / HOP;
  const int minLen = std::max(1, (int)lround(ep.minLength * fr)), bridge = (int)lround(ep.bridge * fr);
  auto reattack = [&](int f, int midi) {
    if (!bp || bp->frames <= 0 || midi < BasicPitch::LOWEST || midi >= BasicPitch::LOWEST + BasicPitch::PITCHES) return false;
    const int g = std::min(f, bp->frames - 1);
    const float o = bp->onsetAt(g, midi);
    if (o <= 0.5f) return false;
    for (int d = -2; d <= 2; d++)
    {
      const int h = std::max(0, std::min(bp->frames - 1, g + d));
      if (d && (bp->onsetAt(h, midi) > o || (d < 0 && bp->onsetAt(h, midi) == o))) return false;
    }
    return true;
  };
  for (int p = 0; p < n; p++)
  {
    for (int f = 0; f < F;)
    {
      if (!act.on[(size_t)f * n + p]) { f++; continue; }
      const int start = f;
      int last = f, g = f + 1;
      bool split = false;
      for (; g < F && g - last <= bridge + 1; g++)
      {
        if (!act.hold[(size_t)g * n + p]) continue;
        if (g - start >= minLen && reattack(g, lo + p)) { split = true; break; }
        last = g;
      }
      const int end = split ? g : last + 1;
      if (end - start >= minLen)
      {
        double sum = 0;
        for (int h = start; h < end; h++) sum += act.S[(size_t)h * n + p];
        out.push_back({start, end, lo + p, (float)(sum / (end - start))});
      }
      f = end;
      if (split) act.on[(size_t)end * n + p] = 1; // the re-attack starts the next note
    }
  }
  std::sort(out.begin(), out.end(), [](const NoteEstimate &a, const NoteEstimate &b) { return a.f0 < b.f0 || (a.f0 == b.f0 && a.pitch < b.pitch); });
  return out;
}

// --- voices -------------------------------------------------------------------------------------

static const double AGE = 2.0, AGE_CAP = 2.0; // semitones of cost per second a strand has been silent, capped (s)
static const double POLYPHONY_SHARE = 0.003;

std::vector<int> assignVoices(const std::vector<NoteEstimate> &notes, double frameRate, int maxVoices)
{
  std::vector<int> voice(notes.size(), -1);
  if (notes.empty() || maxVoices < 1) return voice;
  int F = 0;
  for (const NoteEstimate &n : notes) F = std::max(F, n.f1);
  std::vector<int> cnt(F + 1, 0);
  for (const NoteEstimate &n : notes)
    for (int f = n.f0; f < n.f1; f++) cnt[f]++;
  int sounding = 0;
  for (int c : cnt) sounding += c > 0;
  int nv = 1;
  for (int k = 2; k <= maxVoices; k++)
  {
    int m = 0;
    for (int c : cnt) m += c >= k;
    if (m >= POLYPHONY_SHARE * sounding) nv = k;
  }
  // resting pitch per strand (top first) before it has sung: spread over the notes' range
  std::vector<int> pitches;
  for (const NoteEstimate &n : notes) pitches.push_back(n.pitch);
  std::sort(pitches.begin(), pitches.end());
  std::vector<double> rest(nv);
  for (int v = 0; v < nv; v++)
  {
    const double pos = (1 - (v + 0.5) / nv) * (pitches.size() - 1);
    const int i = (int)pos;
    rest[v] = pitches[i] + (i + 1 < (int)pitches.size() ? (pitches[i + 1] - pitches[i]) * (pos - i) : 0);
  }

  std::vector<int> order(notes.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return notes[a].f0 < notes[b].f0; });
  std::vector<int> strandOf(notes.size(), -1), current(nv, -1); // note sounding on each strand
  std::vector<double> last(nv, -1);
  std::vector<int> silentFrom(nv, -1); // frame the strand fell silent, -1 if it never sang
  for (size_t g = 0; g < order.size();)
  {
    const int t = notes[order[g]].f0;
    std::vector<int> fresh;
    for (; g < order.size() && notes[order[g]].f0 == t; g++) fresh.push_back(order[g]);
    for (int v = 0; v < nv; v++)
      if (current[v] >= 0 && notes[current[v]].f1 <= t) silentFrom[v] = notes[current[v]].f1, current[v] = -1;
    std::vector<int> free;
    for (int v = 0; v < nv; v++)
      if (current[v] < 0) free.push_back(v);
    // more new notes than free strands: the strongest get a strand
    std::sort(fresh.begin(), fresh.end(), [&](int a, int b) { return notes[a].strength > notes[b].strength; });
    if (fresh.size() > free.size()) fresh.resize(free.size());
    std::sort(fresh.begin(), fresh.end(), [&](int a, int b) { return notes[a].pitch > notes[b].pitch; });

    // place as many as fit without crossing, at the lowest cost (k = all of them first, then fewer)
    const int nf = (int)fresh.size(), nfree = (int)free.size();
    std::vector<int> best, bestNotes;
    for (int k = nf; k >= 0 && best.empty() && (k > 0 || nf == 0); k--)
    {
      double bestCost = 1e30;
      for (int nm = 0; nm < (1 << nf); nm++)
      {
        if (__builtin_popcount(nm) != k) continue;
        for (int sm = 0; sm < (1 << nfree); sm++)
        {
          if (__builtin_popcount(sm) != k) continue;
          std::vector<int> sub, combo;
          for (int i = 0; i < nf; i++)
            if (nm >> i & 1) sub.push_back(fresh[i]); // high to low
          for (int i = 0; i < nfree; i++)
            if (sm >> i & 1) combo.push_back(free[i]); // top to bottom
          std::vector<int> pitchOn(nv, -1000);
          for (int v = 0; v < nv; v++)
            if (current[v] >= 0) pitchOn[v] = notes[current[v]].pitch;
          for (int i = 0; i < k; i++) pitchOn[combo[i]] = notes[sub[i]].pitch;
          bool crossing = false;
          int above = 1000;
          for (int v = 0; v < nv; v++)
            if (pitchOn[v] > -1000)
            {
              crossing |= pitchOn[v] > above;
              above = pitchOn[v];
            }
          if (crossing) continue;
          double cost = 0;
          for (int i = 0; i < k; i++)
          {
            const int v = combo[i];
            const double silent = silentFrom[v] < 0 ? AGE_CAP : std::min(AGE_CAP, (t - silentFrom[v]) / frameRate);
            cost += fabs(notes[sub[i]].pitch - (last[v] >= 0 ? last[v] : rest[v])) + AGE * silent;
          }
          if (cost < bestCost) bestCost = cost, best = combo, bestNotes = sub;
        }
      }
    }
    for (size_t i = 0; i < best.size(); i++)
    {
      const int v = best[i], n = bestNotes[i];
      strandOf[n] = v, current[v] = n, last[v] = notes[n].pitch;
    }
  }

  // the strand singing most is the lead
  std::vector<long long> sung(nv, 0);
  for (size_t i = 0; i < notes.size(); i++)
    if (strandOf[i] >= 0) sung[strandOf[i]] += notes[i].f1 - notes[i].f0;
  const int lead = (int)(std::max_element(sung.begin(), sung.end()) - sung.begin());
  std::vector<int> voiceOf(nv);
  for (int v = 0, next = 1; v < nv; v++) voiceOf[v] = v == lead ? 0 : next++;
  for (size_t i = 0; i < notes.size(); i++) voice[i] = strandOf[i] >= 0 ? voiceOf[strandOf[i]] : -1;
  return voice;
}
