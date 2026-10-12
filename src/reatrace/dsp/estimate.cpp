#include "dsp/estimate.h"

#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <thread>

#include "dsp/parallel.h"
#include "fft.h"

static const int SR = 22050, HOP = 256;

NSGTParams estimateNSGTParams() {
    return NSGTParams();
} // 60 bins per octave, gamma 11 Hz: as tested in transcribe

// Divide by the 99th percentile (numpy's linear interpolation) of the frame maxima.
static void normaliseP99(std::vector<float> &roll, int n) {
    const int F = n > 0 ? (int)(roll.size() / n) : 0;
    if (!F) {
        return;
    }
    std::vector<float> mx(F);
    for (int f = 0; f < F; f++) {
        mx[f] = *std::max_element(roll.begin() + (size_t)f * n, roll.begin() + (size_t)(f + 1) * n);
    }
    std::sort(mx.begin(), mx.end());
    const double pos = 0.99 * (F - 1);
    const int i = (int)pos;
    const double v = std::max(1e-12, mx[i] + (i + 1 < F ? (mx[i + 1] - mx[i]) * (pos - i) : 0.0));
    for (float &x : roll) {
        x = (float)(x / v);
    }
}

// --- harmonic-template NMF (transcribe mf0.templates / nmf / adapt) ------------------------------

static const int SUB = 3;          // atoms per semitone
static const int NH = 12;          // partials per comb
static const double ROLLOFF = 0.6; // partial h has amplitude h^-ROLLOFF
static const double WIDTH = 0.6;   // partial width in bins at 36 bins per octave
static const double LAM = 0.02;    // L1 penalty relative to frame energy
static const int ITERS = 150, OUTER = 3, INNER = 25;
static const int REGION = 6; // semitones sharing adapted partial amplitudes
static const double CLAMP = 1.25, SOLO = 0.6;
static const int CHUNK = 1024; // frames per matrix product, to bound memory

std::vector<float> nmfRoll(const Spectrogram &s, int lo, int hi, bool adapt,
                           const std::function<bool(double)> &progress, int harmonics) {
    const int n = hi - lo + 1, F = s.frames, bpo = s.p.binsPerOctave;
    std::vector<float> roll((size_t)std::max(0, F) * std::max(0, n), 0);
    const int b0 = std::max(0, (int)lround(s.binOfPitch(36))),
              B = s.bins - b0; // the axis starts at C2, as in transcribe
    if (F <= 0 || B <= 0 || n <= 0) {
        return roll;
    }
    const int pmin = std::min(45, lo - 10), pmax = std::max(86, hi + 2),
              P = (pmax - pmin) * SUB + 1;
    const double width = WIDTH * bpo / 36.0;
    auto pitchOf = [&](int p) { return pmin + (double)p / SUB; };

    // Gaussian partials: part[(b * P + p) * NH + h - 1]
    std::vector<float> part((size_t)B * P * NH, 0);
    for (int p = 0; p < P; p++) {
        for (int h = 1; h <= std::min(NH, harmonics); h++) {
            const double c = s.binOfPitch(pitchOf(p) + 12 * log2((double)h)) - b0;
            if (c >= B) {
                break;
            }
            for (int b = std::max(0, (int)floor(c - 5 * width));
                 b <= std::min(B - 1, (int)ceil(c + 5 * width)); b++) {
                part[((size_t)b * P + p) * NH + h - 1] =
                    (float)exp(-0.5 * ((b - c) / width) * ((b - c) / width));
            }
        }
    }
    // broadband atoms (half-octave bumps) soak up breath and consonants so low combs don't
    std::vector<std::vector<float>> noise;
    for (int c = 0; c < B; c += bpo / 2) {
        std::vector<float> col(B);
        double sum = 0;
        for (int b = 0; b < B; b++) {
            sum += col[b] = (float)exp(-0.5 * ((b - c) / (bpo / 3.0)) * ((b - c) / (bpo / 3.0)));
        }
        for (float &v : col) {
            v = (float)(v / sum);
        }
        noise.push_back(col);
    }
    const int K = P + (int)noise.size();
    const int regions = (P - 1) / (SUB * REGION) + 1;
    auto reg = [](int p) { return p / (SUB * REGION); };
    std::vector<double> init(NH);
    for (int h = 0; h < NH; h++) {
        init[h] = pow(h + 1.0, -ROLLOFF);
    }
    std::vector<double> A((size_t)regions * NH);
    for (int r = 0; r < regions; r++) {
        std::copy(init.begin(), init.end(), A.begin() + (size_t)r * NH);
    }

    auto V = [&](int f, int b) { return s.mag[(size_t)f * s.bins + b0 + b]; };
    const float eps = 1e-9f;
    double mean = 0;
    std::vector<float> pen(F);
    for (int f = 0; f < F; f++) {
        double sum = 0;
        for (int b = 0; b < B; b++) {
            sum += V(f, b);
        }
        mean += sum, pen[f] = (float)(LAM * (sum + eps) / B * 50);
    }
    mean /= (double)F * B;

    std::vector<float> H((size_t)F * K, (float)(mean + eps)), W((size_t)B * K), norm(P);
    std::vector<float> WH((size_t)CHUNK * B), NUM((size_t)CHUNK * K);
    auto buildW = [&] {
        for (int p = 0; p < P; p++) {
            double sum = 0;
            for (int b = 0; b < B; b++) {
                double v = 0;
                const float *pp = &part[((size_t)b * P + p) * NH];
                for (int h = 0; h < NH; h++) {
                    v += pp[h] * A[(size_t)reg(p) * NH + h];
                }
                W[(size_t)b * K + p] = (float)v, sum += v;
            }
            norm[p] = (float)(sum + eps);
            for (int b = 0; b < B; b++) {
                W[(size_t)b * K + p] /= norm[p];
            }
        }
        for (size_t j = 0; j < noise.size(); j++) {
            for (int b = 0; b < B; b++) {
                W[(size_t)b * K + P + j] = noise[j][b];
            }
        }
    };
    // WH = V / (H W^T) for frames c0..c0+fc
    auto ratio = [&](int c0, int fc) {
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, fc, B, K, 1.0f, &H[(size_t)c0 * K], K,
                    W.data(), K, 0.0f, WH.data(), B);
        for (int f = 0; f < fc; f++) {
            float *row = &WH[(size_t)f * B];
            vDSP_vsadd(row, 1, &eps, row, 1, B);
            vDSP_vdiv(row, 1, &s.mag[(size_t)(c0 + f) * s.bins + b0], 1, row, 1,
                      B); // V / (WH + eps)
        }
    };
    int done = 0;
    // KL multiplicative updates with W fixed; W's columns sum to 1
    auto iterate = [&](int count) {
        for (int it = 0; it < count; it++, done++) {
            for (int c0 = 0; c0 < F; c0 += CHUNK) {
                const int fc = std::min(CHUNK, F - c0);
                ratio(c0, fc);
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, fc, K, B, 1.0f, WH.data(), B,
                            W.data(), K, 0.0f, NUM.data(), K);
                for (int f = 0; f < fc; f++) {
                    float *h = &H[(size_t)(c0 + f) * K];
                    const float *num = &NUM[(size_t)f * K], d = 1 + pen[c0 + f];
                    for (int k = 0; k < K; k++) {
                        h[k] *= num[k] / d;
                    }
                }
            }
            if (progress && !progress((double)done / ITERS)) {
                return false;
            }
        }
        return true;
    };
    // Re-estimates the partial amplitudes per region from solo-ish frames (Vincent, Bertin & Badeau 2010).
    auto adaptStep = [&] {
        std::vector<float> RH((size_t)B * P, 0), Hm((size_t)CHUNK * P);
        std::vector<double> hsum(P, 0);
        for (int c0 = 0; c0 < F; c0 += CHUNK) {
            const int fc = std::min(CHUNK, F - c0);
            ratio(c0, fc);
            for (int f = 0; f < fc; f++) {
                const float *h = &H[(size_t)(c0 + f) * K];
                double sum = 0, best = 0;
                for (int p = 0; p < P; p++) {
                    sum += h[p];
                    best = std::max(best, (double)(p > 0 ? h[p - 1] : 0) + h[p] +
                                              (p + 1 < P ? h[p + 1] : 0));
                }
                const bool solo = best / (sum + eps) >= SOLO;
                for (int p = 0; p < P; p++) {
                    Hm[(size_t)f * P + p] = solo ? h[p] : 0, hsum[p] += solo ? h[p] : 0;
                }
            }
            cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans, B, P, fc, 1.0f, WH.data(), B,
                        Hm.data(), P, 1.0f, RH.data(), P);
        }
        std::vector<double> nr((size_t)regions * NH, 0), dr((size_t)regions * NH, 0);
        for (int p = 0; p < P; p++) {
            for (int b = 0; b < B; b++) {
                const float *pp = &part[((size_t)b * P + p) * NH];
                const double rh = RH[(size_t)b * P + p] / norm[p], hn = hsum[p] / norm[p];
                for (int h = 0; h < NH; h++) {
                    nr[(size_t)reg(p) * NH + h] += pp[h] * rh,
                        dr[(size_t)reg(p) * NH + h] += pp[h] * hn;
                }
            }
        }
        for (size_t i = 0; i < A.size(); i++) {
            A[i] *= (nr[i] + eps) / (dr[i] + eps);
        }
        for (int r = 0; r < regions; r++) {
            const double a0 = std::max((double)eps, A[(size_t)r * NH]);
            for (int h = 0; h < NH; h++) {
                A[(size_t)r * NH + h] /= a0;
            }
        }
        if (regions > 2) // smooth across regions in the log domain
        {
            std::vector<double> L(A.size());
            for (size_t i = 0; i < A.size(); i++) {
                L[i] = log(std::max((double)eps, A[i]));
            }
            for (int r = 1; r + 1 < regions; r++) {
                for (int h = 0; h < NH; h++) {
                    A[(size_t)r * NH + h] =
                        exp((L[(size_t)(r - 1) * NH + h] + 2 * L[(size_t)r * NH + h] +
                             L[(size_t)(r + 1) * NH + h]) /
                            4);
                }
            }
        }
        for (int r = 0; r < regions; r++) {
            for (int h = 0; h < NH; h++) {
                double &a = A[(size_t)r * NH + h];
                a = std::max(init[h] / CLAMP, std::min(init[h] * CLAMP, a));
            }
        }
    };

    const int outer = adapt ? OUTER : 0;
    for (int it = 0;; it++) {
        buildW();
        if (!iterate(it < outer ? INNER : ITERS - outer * INNER)) {
            return roll;
        }
        if (it == outer) {
            break;
        }
        adaptStep();
    }

    // pool the three atoms of each semitone
    for (int f = 0; f < F; f++) {
        for (int m = lo; m <= hi; m++) {
            double sum = 0;
            for (int j = SUB * (m - pmin) - 1; j <= SUB * (m - pmin) + 1; j++) {
                if (j >= 0 && j < P) {
                    sum += H[(size_t)f * K + j];
                }
            }
            roll[(size_t)f * n + m - lo] = (float)sum;
        }
    }
    normaliseP99(roll, n);
    return roll;
}

// --- enhanced summary autocorrelation (transcribe bench/approaches/a04_sacf.py) ------------------

static const int SACF_BANDS = 16;
static const double SACF_LOW = 80, SACF_HIGH = 6000; // band edges, Hz
static const double SACF_K = 0.3;                    // generalised ACF: ifft(|X|^K)
static const int SACF_NFFT = 2048, SACF_UP = 4;      // 93 ms frames; lag oversampling
static const int SACF_MARGIN = SR / 2;               // filter run-in around each chunk, samples

struct Biquad {
    double b0, b1, b2, a1, a2;
};

// scipy.signal.butter(2, [lo, hi], 'band'): the analog prototype moved to the band and through the
// bilinear transform, as two second-order sections.
static void butterBandpass(double lo, double hi, Biquad out[2]) {
    typedef std::complex<double> C;
    const double fs = 2, w1 = 2 * fs * tan(M_PI * (lo / (SR / 2.0)) / fs),
                 w2 = 2 * fs * tan(M_PI * (hi / (SR / 2.0)) / fs);
    const double bw = w2 - w1, wo = sqrt(w1 * w2), fs2 = 2 * fs;
    const C proto[2] = {-std::exp(C(0, -M_PI / 4)), -std::exp(C(0, M_PI / 4))};
    C poles[4];
    for (int i = 0; i < 2; i++) {
        const C pl = proto[i] * (bw / 2), r = std::sqrt(pl * pl - wo * wo);
        poles[i] = pl + r, poles[2 + i] = pl - r;
    }
    C den = 1;
    for (C &p : poles) {
        den *= fs2 - p, p = (fs2 + p) / (fs2 - p);
    }
    const double k =
        bw * bw * std::real(C(fs2 * fs2) / den); // zeros: two at s=0 (z=1), two at infinity (z=-1)
    for (size_t s = 0; s < 2; s++) {
        const C a = poles[2 * s], b = poles[2 * s + 1];
        const double g = s == 0 ? k : 1;
        out[s] = {g, 0, -g, -std::real(a + b), std::real(a * b)};
    }
}

static void biquadRun(const Biquad &q, float *x, int n, int step) {
    double z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        float &v = x[(ptrdiff_t)i * step]; // signed: the backward pass steps by -1
        const double in = v, y = q.b0 * in + z1;
        z1 = q.b1 * in - q.a1 * y + z2;
        z2 = q.b2 * in - q.a2 * y;
        v = (float)y;
    }
}

// zero-phase: forward, then backward
static void filtfilt(const Biquad sos[2], std::vector<float> &x) {
    const int n = (int)x.size();
    if (!n) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        biquadRun(sos[s], x.data(), n, 1);
    }
    for (int s = 0; s < 2; s++) {
        biquadRun(sos[s], x.data() + n - 1, n, -1);
    }
}

std::vector<float> sacfRoll(const std::vector<float> &mono, int frames, int lo, int hi,
                            const std::function<bool(double)> &progress) {
    const int n = hi - lo + 1, F = std::max(0, frames);
    std::vector<float> roll((size_t)F * std::max(0, n), 0);
    if (!F || n <= 0) {
        return roll;
    }
    static bool fftInit = (WDL_fft_init(), true);
    (void)fftInit;

    Biquad bands[SACF_BANDS][2];
    for (int i = 0; i < SACF_BANDS; i++) {
        butterBandpass(SACF_LOW * pow(SACF_HIGH / SACF_LOW, (double)i / SACF_BANDS),
                       SACF_LOW * pow(SACF_HIGH / SACF_LOW, (double)(i + 1) / SACF_BANDS),
                       bands[i]);
    }
    const int maxlag = (int)(SR / 70.0 * SACF_UP), NI = SACF_NFFT * SACF_UP;
    // lags of each semitone: within +-50 cents, else the nearest lag
    std::vector<int> la(n), lb(n);
    for (int k = lo; k <= hi; k++) {
        int a = maxlag, b = 0, nearest = 1;
        for (int l = 1; l < maxlag; l++) {
            const double m = 69 + 12 * log2((double)SR * SACF_UP / l / 440);
            if (fabs(m - k) <= 0.5) {
                a = std::min(a, l), b = std::max(b, l);
            }
            if (fabs(m - k) < fabs(69 + 12 * log2((double)SR * SACF_UP / nearest / 440) - k)) {
                nearest = l;
            }
        }
        if (a > b) {
            a = b = nearest;
        }
        la[k - lo] = a, lb[k - lo] = b;
    }
    std::vector<float> hann(SACF_NFFT);
    for (int i = 0; i < SACF_NFFT; i++) {
        hann[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / SACF_NFFT));
    }

    const int chunk = 512, chunks = (F + chunk - 1) / chunk;
    std::atomic<int> next{0}, finished{0};
    std::atomic<bool> cancel{false};
    auto worker = [&] {
        std::vector<float> x, acc((size_t)chunk * maxlag), st(maxlag);
        std::vector<WDL_FFT_REAL> buf(SACF_NFFT), ibuf(NI);
        const int *perm = WDL_fft_permute_tab(SACF_NFFT / 2), *iperm = WDL_fft_permute_tab(NI / 2);
        std::vector<float> pw(SACF_NFFT / 2 + 1), expo(pw.size(), (float)(SACF_K / 2));
        int bins = (int)pw.size();
        for (int ci = 0; !cancel && (ci = next++) < chunks;) {
            const int c0 = ci * chunk, fc = std::min(chunk, F - c0);
            const long long s0 = (long long)c0 * HOP - SACF_NFFT / 2 - SACF_MARGIN;
            const int len = (fc - 1) * HOP + SACF_NFFT + 2 * SACF_MARGIN;
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (int band = 0; band < SACF_BANDS; band++) {
                x.assign(len, 0);
                for (int i = 0; i < len; i++) {
                    const long long j = s0 + i;
                    if (j >= 0 && j < (long long)mono.size()) {
                        x[i] = mono[j];
                    }
                }
                filtfilt(bands[band], x);
                for (int f = 0; f < fc; f++) {
                    const float *seg =
                        &x[(size_t)f * HOP + SACF_MARGIN]; // frame centred on (c0+f)*HOP
                    for (int i = 0; i < SACF_NFFT; i++) {
                        buf[i] = seg[i] * hann[i];
                    }
                    WDL_real_fft(buf.data(), SACF_NFFT, 0);
                    const WDL_FFT_COMPLEX *X = (const WDL_FFT_COMPLEX *)buf.data();
                    // |X|^K = (|X|^2)^(K/2), vectorised
                    pw[0] = X[0].re * X[0].re,
                    pw[SACF_NFFT / 2] = X[0].im * X[0].im; // Nyquist is packed into bin 0
                    for (int k = 1; k < SACF_NFFT / 2; k++) {
                        pw[k] = X[perm[k]].re * X[perm[k]].re + X[perm[k]].im * X[perm[k]].im;
                    }
                    vvpowf(pw.data(), expo.data(), pw.data(), &bins);
                    std::fill(ibuf.begin(), ibuf.end(), 0);
                    WDL_FFT_COMPLEX *Y = (WDL_FFT_COMPLEX *)ibuf.data();
                    for (int k = 0; k <= SACF_NFFT / 2; k++) {
                        Y[iperm[k]].re = pw[k];
                    }
                    WDL_real_fft(ibuf.data(), NI, 1);
                    float *a = &acc[(size_t)f * maxlag];
                    for (int l = 0; l < maxlag; l++) {
                        a[l] += (float)ibuf[l];
                    }
                }
            }
            for (int f = 0; f < fc; f++) {
                float *S = &acc[(size_t)f * maxlag];
                for (int l = 0; l < maxlag; l++) {
                    S[l] = std::max(0.0f, S[l]);
                }
                // "enhanced": remove the peaks at multiples of each period (sub-octaves)
                for (int fct = 2; fct <= 3; fct++) {
                    for (int l = 0; l < maxlag; l++) {
                        const double pos = (double)l / fct;
                        const int i = (int)pos;
                        st[l] = S[i] + (i + 1 < maxlag ? (S[i + 1] - S[i]) * (float)(pos - i) : 0);
                    }
                    for (int l = 0; l < maxlag; l++) {
                        S[l] = std::max(0.0f, S[l] - st[l]);
                    }
                }
                for (int k = 0; k < n; k++) {
                    float m = 0;
                    for (int l = la[k]; l <= lb[k]; l++) {
                        m = std::max(m, S[l]);
                    }
                    roll[(size_t)(c0 + f) * n + k] = m;
                }
            }
            const int d = ++finished;
            (void)d;
        }
    };
    const int threads = std::max(1, std::min(chunks, (int)std::thread::hardware_concurrency()));
    // the calling thread reports progress until the chunks are done, or a worker failed
    parallel(
        threads, cancel, [&](int) { worker(); },
        [&] {
            while (finished < chunks && !cancel) {
                if (progress && !progress((double)finished / chunks)) {
                    cancel = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    if (cancel) {
        return roll;
    }
    normaliseP99(roll, n);
    return roll;
}

// --- activity and segmentation (transcribe notes.frame_active; hysteresis instead of a grid) -----

struct Activity {
    std::vector<float> S;                // median filtered and gated
    std::vector<unsigned char> on, hold; // starts a note / keeps one going
};

// Median over 5 frames along time ("reflect" edges as scipy), gate, local pitch peaks, absolute and
// relative thresholds (halved for `hold`), and no upper octave weaker than OCT x the note below.
static Activity activity(const std::vector<float> &roll, int F, int n,
                         const std::vector<unsigned char> *gate, double absT, double rel,
                         double oct) {
    Activity a;
    a.S.assign(roll.size(), 0), a.on.assign(roll.size(), 0), a.hold.assign(roll.size(), 0);
    auto refl = [F](int i) { return i < 0 ? -i - 1 : i >= F ? 2 * F - i - 1 : i; };
    float w[5];
    for (int f = 0; f < F; f++) {
        for (int p = 0; p < n; p++) {
            for (int d = -2; d <= 2; d++) {
                w[d + 2] = roll[(size_t)refl(std::max(-F, std::min(2 * F - 1, f + d))) * n + p];
            }
            std::nth_element(w, w + 2, w + 5);
            a.S[(size_t)f * n + p] = gate && !(*gate)[(size_t)f * n + p] ? 0 : w[2];
        }
    }
    for (int f = 0; f < F; f++) {
        const float *S = &a.S[(size_t)f * n];
        const float mx = *std::max_element(S, S + n);
        for (int level = 0; level < 2; level++) {
            const double k = level ? 0.5 : 1;
            unsigned char *act = level ? &a.hold[(size_t)f * n] : &a.on[(size_t)f * n];
            for (int p = 0; p < n; p++) {
                const bool peak = (p == 0 || S[p] >= S[p - 1]) && (p + 1 == n || S[p] >= S[p + 1]);
                act[p] = peak && S[p] > absT * k && S[p] > rel * k * mx;
            }
            for (int p = n - 1; p >= 12; p--) {
                if (act[p] && act[p - 12] && S[p] < oct * S[p - 12]) {
                    act[p] = 0; // the lower note's 2nd partial
                }
            }
        }
    }
    return a;
}

// --- octave doublings (transcribe refine.octave_doublings) ----------------------------------------

static const int RA_NFFT = 2048, RA_BINS = 520; // reassigned spectrogram up to ~5.6 kHz
static const double TOL = 0.45; // semitones a partial track may stray from its nominal frequency
static const double ADD = 0.9;  // even-partial surplus (log units) that adds the octave above
static const double FM_VETO = 0.8,
                    SELF_FM =
                        0.5; // common fate: the upper F0 moving with the lower note's partials
static const double CHUNK_S =
    0.25; // seconds per surplus measurement (transcribe: an 8th-note slot)
static const float REATTACK_DIP =
    0.7f; // a re-attack needs the level before it below this share of the level after

// Reassigned spectrogram (librosa's: instantaneous frequency from the derivative window, magnitude
// of the plain STFT), computed only for the frames asked for.
struct Partials {
    const std::vector<float> &y;
    int frames;
    std::vector<std::vector<float>> F, M;
    std::vector<float> win, dwin;
    std::vector<WDL_FFT_REAL> a, b;

    Partials(const std::vector<float> &y, int frames)
        : y(y), frames(frames), F(frames), M(frames), win(RA_NFFT), dwin(RA_NFFT), a(RA_NFFT),
          b(RA_NFFT) {
        for (int i = 0; i < RA_NFFT; i++) {
            win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / RA_NFFT));
        }
        for (int i = 0; i < RA_NFFT; i++) {
            dwin[i] = 0.5f * (win[(i + 1) % RA_NFFT] -
                              win[(i + RA_NFFT - 1) % RA_NFFT]); // cyclic gradient
        }
    }

    void ensure(int f) {
        if (!F[f].empty()) {
            return;
        }
        for (int i = 0; i < RA_NFFT; i++) {
            const long long j = (long long)f * HOP - RA_NFFT / 2 + i;
            const float v = j >= 0 && j < (long long)y.size() ? y[j] : 0;
            a[i] = v * win[i], b[i] = v * dwin[i];
        }
        WDL_real_fft(a.data(), RA_NFFT, 0);
        WDL_real_fft(b.data(), RA_NFFT, 0);
        const WDL_FFT_COMPLEX *X = (const WDL_FFT_COMPLEX *)a.data(),
                              *D = (const WDL_FFT_COMPLEX *)b.data();
        const int *perm = WDL_fft_permute_tab(RA_NFFT / 2);
        F[f].assign(RA_BINS, 0), M[f].assign(RA_BINS, 0);
        for (int k = 1; k < RA_BINS; k++) {
            // WDL's real FFT is twice numpy's; halve so the power threshold means the same
            const std::complex<double> h(0.5 * X[perm[k]].re, 0.5 * X[perm[k]].im),
                d(0.5 * D[perm[k]].re, 0.5 * D[perm[k]].im);
            const double mag = std::abs(h);
            M[f][k] = (float)mag;
            if (mag * mag < 1e-6) {
                continue; // librosa's ref_power: no frequency (0, as transcribe's nan_to_num)
            }
            const double fr = (double)k * SR / RA_NFFT - std::imag(d / h) * 0.5 * SR / M_PI;
            F[f][k] = (float)std::max(0.0, std::min(SR / 2.0, fr));
        }
    }

    // log2 frequency (NaN where nothing is near) and log magnitude of the strongest bin near f0, frames a..b-1
    void track(double f0, int a0, int b0, std::vector<double> &t, std::vector<double> &m,
               double tol = TOL) {
        b0 = std::min(b0, frames);
        t.clear(), m.clear();
        const double bw = (double)SR / RA_NFFT;
        const int k0 = std::max(0, (int)floor(f0 * pow(2, -tol / 12) / bw - 1));
        const int k1 = std::min(RA_BINS - 1, (int)ceil(f0 * pow(2, tol / 12) / bw + 1));
        for (int f = std::max(0, a0); f < b0; f++) {
            ensure(f);
            int best = k0;
            for (int k = k0; k <= k1; k++) {
                if (M[f][k] > M[f][best]) {
                    best = k;
                }
            }
            const double fr = F[f][best];
            const bool ok = fr > 0 && fabs(12 * log2(std::max(fr, 1e-3) / f0)) < tol;
            t.push_back(ok ? log2(std::max(fr, 1e-3)) : NAN), m.push_back(log(M[f][best] + 1e-6));
        }
    }

    // log mean magnitude of partials 1..nh (up to 5 kHz)
    std::vector<double> harmonicAmps(double f0, int a0, int b0, int nh = 10) {
        std::vector<double> out(nh, NAN), t, m;
        for (int h = 1; h <= nh && h * f0 <= 5000; h++) {
            track(h * f0, a0, b0, t, m);
            double sum = 0;
            for (double v : m) {
                sum += exp(v);
            }
            if (!m.empty()) {
                out[h - 1] = log(sum / (double)m.size() + 1e-6);
            }
        }
        return out;
    }
};

static double hzOf(int midi) { return 440 * pow(2.0, (midi - 69) / 12.0); }

// How far the even partials stick out over their odd neighbours (log units); NaN if unmeasurable.
static double evenSurplus(const std::vector<double> &amps) {
    double sum = 0;
    int n = 0;
    for (int h : {2, 4, 6, 8}) {
        if (h < (int)amps.size() && std::isfinite(amps[h - 2]) && std::isfinite(amps[h - 1]) &&
            std::isfinite(amps[h])) {
            sum += amps[h - 1] - 0.5 * (amps[h - 2] + amps[h]), n++;
        }
    }
    return n ? sum / n : NAN;
}

// Gaps filled, a quadratic trend removed, smoothed over 3 frames; false if too little is voiced.
static bool detrend(const std::vector<double> &x, std::vector<double> &out) {
    const int n = (int)x.size();
    std::vector<int> ok;
    for (int i = 0; i < n; i++) {
        if (std::isfinite(x[i])) {
            ok.push_back(i);
        }
    }
    if ((int)ok.size() < 8) {
        return false;
    }
    std::vector<double> v(n);
    for (int i = 0, j = 0; i < n; i++) {
        while (j + 1 < (int)ok.size() && ok[j + 1] <= i) {
            j++;
        }
        if (i <= ok.front()) {
            v[i] = x[ok.front()];
        } else if (i >= ok.back()) {
            v[i] = x[ok.back()];
        } else {
            const int a = ok[j], b = ok[j + 1];
            v[i] = a == i ? x[a] : x[a] + (x[b] - x[a]) * (i - a) / (b - a);
        }
    }
    // least-squares quadratic, centred for conditioning
    double S[5] = {}, T[3] = {};
    const double c = (n - 1) / 2.0;
    for (int i = 0; i < n; i++) {
        const double u = i - c;
        double p = 1;
        for (int k = 0; k < 5; k++) {
            S[k] += p, p *= u;
        }
        T[0] += v[i], T[1] += v[i] * u, T[2] += v[i] * u * u;
    }
    double A[3][4] = {{S[0], S[1], S[2], T[0]}, {S[1], S[2], S[3], T[1]}, {S[2], S[3], S[4], T[2]}};
    for (int i = 0; i < 3; i++) {
        for (int r = i + 1; r < 3; r++) {
            const double q = A[r][i] / A[i][i];
            for (int k = i; k < 4; k++) {
                A[r][k] -= q * A[i][k];
            }
        }
    }
    double coef[3];
    for (int i = 2; i >= 0; i--) {
        double sum = A[i][3];
        for (int k = i + 1; k < 3; k++) {
            sum -= A[i][k] * coef[k];
        }
        coef[i] = sum / A[i][i];
    }
    std::vector<double> d(n);
    for (int i = 0; i < n; i++) {
        const double u = i - c;
        d[i] = v[i] - (coef[0] + coef[1] * u + coef[2] * u * u);
    }
    out.assign(n, 0);
    auto at = [&](int i) { return d[i < 0 ? -i - 1 : i >= n ? 2 * n - i - 1 : i]; };
    for (int i = 0; i < n; i++) {
        out[i] = (at(i - 1) + at(i) + at(i + 1)) / 3;
    }
    return true;
}

// Pearson correlation of two frequency tracks, NaN if either is mostly unvoiced or flat.
static double trackCorr(const std::vector<double> &x, const std::vector<double> &y) {
    auto voiced = [](const std::vector<double> &v) {
        int n = 0;
        for (double e : v) {
            n += std::isfinite(e);
        }
        return v.empty() ? 0.0 : (double)n / (double)v.size();
    };
    std::vector<double> a, b;
    if (voiced(x) < 0.6 || voiced(y) < 0.6 || !detrend(x, a) || !detrend(y, b)) {
        return NAN;
    }
    const int n = (int)a.size();
    double ma = 0, mb = 0;
    for (int i = 0; i < n; i++) {
        ma += a[i] / n, mb += b[i] / n;
    }
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) {
        sab += (a[i] - ma) * (b[i] - mb), saa += (a[i] - ma) * (a[i] - ma),
            sbb += (b[i] - mb) * (b[i] - mb);
    }
    if (sqrt(saa / n) < 1e-7 || sqrt(sbb / n) < 1e-7) {
        return NAN;
    }
    return sab / sqrt(saa * sbb);
}

// Common fate: the upper note's F0 wobbles with the lower note's 1st or 3rd partial, which it can't
// share, while those two move together: it is the lower voice's own overtone.
static bool isOvertone(Partials &P, const NoteEstimate &up, const NoteEstimate &low) {
    const int a = std::max(up.f0, low.f0) + 2, b = std::min(up.f1, low.f1) - 2;
    if (b - a < 10) {
        return false;
    }
    const double fl = hzOf(low.pitch);
    std::vector<double> tu, t1, t3, m;
    P.track(hzOf(up.pitch), a, b, tu, m);
    P.track(fl, a, b, t1, m);
    P.track(3 * fl, a, b, t3, m);
    const double c1 = trackCorr(tu, t1), c3 = trackCorr(tu, t3), self = trackCorr(t1, t3);
    double fm = -2;
    if (std::isfinite(c1)) {
        fm = std::max(fm, c1);
    }
    if (std::isfinite(c3)) {
        fm = std::max(fm, c3);
    }
    return fm > -2 && std::isfinite(self) && fm > FM_VETO && self > SELF_FM;
}

// A voice an octave above another is folded into the lower note by the harmonic NMF; it shows as
// even partials standing out over their odd neighbours. Adds the octave above where they do, unless
// common fate says it is the lower voice's overtone; removes a detected upper octave that is one.
static std::vector<NoteEstimate> octaveDoublings(const std::vector<NoteEstimate> &raw, Partials &P,
                                                 int hi, double frameRate) {
    std::vector<NoteEstimate> add;
    std::vector<bool> drop(raw.size(), false);
    auto overlaps = [](const NoteEstimate &a, const NoteEstimate &b) {
        return std::min(a.f1, b.f1) > std::max(a.f0, b.f0);
    };
    const int L = std::max(4, (int)lround(CHUNK_S * frameRate));
    for (const NoteEstimate &n : raw) {
        if (n.pitch + 12 > hi) {
            continue;
        }
        const double f0 = hzOf(n.pitch);
        std::vector<double> E;
        std::vector<int> starts;
        for (int c = n.f0; c < n.f1; c += L) {
            const int a = c + 1, b = std::min(n.f1, c + L) - 1;
            E.push_back(b > a ? evenSurplus(P.harmonicAmps(f0, a, std::max(b, a + 2))) : NAN);
            starts.push_back(c);
        }
        for (size_t s = 0; s < E.size();) {
            if (!(E[s] >= ADD)) {
                s++;
                continue;
            }
            size_t e = s;
            while (e < E.size() && E[e] >= ADD) {
                e++;
            }
            const NoteEstimate up = {
                starts[s], e < E.size() ? starts[e] : n.f1, n.pitch + 12, n.strength * 0.5f
            };
            bool exists = false;
            for (const NoteEstimate &o : raw) {
                exists |= o.pitch == up.pitch && overlaps(o, up);
            }
            if (!exists && !isOvertone(P, up, n)) {
                add.push_back(up);
            }
            s = e;
        }
    }
    for (size_t i = 0; i < raw.size(); i++) {
        for (const NoteEstimate &low : raw) {
            if (low.pitch == raw[i].pitch - 12 && overlaps(raw[i], low) &&
                isOvertone(P, raw[i], low)) {
                drop[i] = true;
                break;
            }
        }
    }
    std::vector<NoteEstimate> out;
    for (size_t i = 0; i < raw.size(); i++) {
        if (!drop[i]) {
            out.push_back(raw[i]);
        }
    }
    out.insert(out.end(), add.begin(), add.end());
    return out;
}

// --- front ends for the vote (transcribe mf0.FRONT_ENDS, notes.VOTERS) --------------------------

static Spectrogram axisFromC2(int bpo, int frames) {
    Spectrogram s;
    s.p.sampleRate = SR, s.p.fmin = hzOf(36), s.p.fmax = hzOf(36) * 128, s.p.binsPerOctave = bpo,
    s.p.hop = HOP, s.p.gamma = 0;
    s.bins = bpo * 7, s.frames = frames;
    s.mag.assign((size_t)s.bins * frames, 0);
    return s;
}

// Fourier transform of a Hann window, normalised to 1 at 0; u is frequency x window length.
static double hannSpectrum(double u) {
    if (fabs(u) < 1e-9) {
        return 1;
    }
    if (fabs(fabs(u) - 1) < 1e-9) {
        return 0.5;
    }
    return sin(M_PI * u) / (M_PI * u) / (1 - u * u);
}

// Like computeNSGT: one FFT per block, each bin's window spectrum multiplied in, then a small inverse
// FFT decimated to the hop. Blocks are 2^17 samples (Accelerate's FFT) since a long CQT window at C2
// is over a second.
Spectrogram computeCQT(const std::vector<float> &x, int bpo, double filterScale,
                       const std::function<bool(double)> &progress) {
    const int frames = (int)(x.size() / HOP) + 1;
    Spectrogram s = axisFromC2(bpo, frames);
    const int LOG2N = 17, N = 1 << LOG2N;
    const double r = pow(2.0, 1.0 / bpo),
                 alpha = (r * r - 1) / (r * r + 1); // librosa's relative bandwidth
    struct Bin {
        int k0 = 0, k1 = 0, M = 0, logM = 0, step = 0;
        std::vector<float> w;
        float scale = 0;
    };
    std::vector<Bin> bins(s.bins);
    double maxL = 0;
    int maxM = 0;
    for (int b = 0; b < s.bins; b++) {
        const double f = s.p.fmin * pow(2.0, (double)b / bpo), L = filterScale * SR / (alpha * f),
                     c = f * N / SR;
        maxL = std::max(maxL, L);
        Bin &bn = bins[b];
        const double reach = 4.0 * N / L; // main lobe and the first sidelobes
        bn.k0 = std::max(1, (int)floor(c - reach)),
        bn.k1 = std::min(N / 2 - 1, (int)ceil(c + reach));
        for (int k = bn.k0; k <= bn.k1; k++) {
            bn.w.push_back((float)hannSpectrum((k - c) * L / N));
        }
        bn.M = N / HOP, bn.logM = LOG2N - 8;
        while (bn.M < bn.k1 - bn.k0 + 1) {
            bn.M *= 2, bn.logM++;
        }
        bn.step = bn.M * HOP / N;
        bn.scale =
            (float)(sqrt(L) / 2 / N); // 1/N: a sine reads its amplitude; sqrt(L)/2: librosa's scale
        maxM = std::max(maxM, bn.M);
    }
    const int margin = ((int)ceil(maxL / 2 / HOP) + 1) * HOP,
              framesPerBlock = (N - 2 * margin) / HOP;
    FFTSetup setup = vDSP_create_fftsetup(LOG2N, kFFTRadix2);
    std::vector<float> block(N), re(N / 2), im(N / 2), sre(maxM), sim(maxM);
    DSPSplitComplex X = {re.data(), im.data()}, Y = {sre.data(), sim.data()};
    for (int first = 0; first < frames; first += framesPerBlock) {
        const long long start = (long long)first * HOP - margin;
        for (int i = 0; i < N; i++) {
            const long long j = start + i;
            block[i] = j >= 0 && j < (long long)x.size() ? x[j] : 0;
        }
        vDSP_ctoz((const DSPComplex *)block.data(), 2, &X, 1, N / 2);
        vDSP_fft_zrip(setup, &X, 1, LOG2N, kFFTDirection_Forward); // twice the DFT, as WDL's
        const int nf = std::min(framesPerBlock, frames - first);
        for (int b = 0; b < s.bins; b++) {
            const Bin &bn = bins[b];
            std::fill(sre.begin(), sre.begin() + bn.M, 0.0f),
                std::fill(sim.begin(), sim.begin() + bn.M, 0.0f);
            for (int k = bn.k0; k <= bn.k1; k++) {
                sre[k - bn.k0] = re[k] * bn.w[k - bn.k0], sim[k - bn.k0] = im[k] * bn.w[k - bn.k0];
            }
            vDSP_fft_zip(setup, &Y, 1, bn.logM, kFFTDirection_Inverse);
            for (int i = 0; i < nf; i++) {
                const int j = (margin / HOP + i) * bn.step;
                s.mag[(size_t)(first + i) * s.bins + b] = hypotf(sre[j], sim[j]) * bn.scale;
            }
        }
        if (progress && !progress((double)(first + nf) / frames)) {
            break;
        }
    }
    vDSP_destroy_fftsetup(setup);
    return s;
}

Spectrogram computeCFP(const std::vector<float> &x, int bpo) {
    static bool fftInit = (WDL_fft_init(), true);
    (void)fftInit;
    const int NF = 4096, frames = (int)(x.size() / HOP) + 1;
    const double G_SPEC = 0.24, G_CEPS = 0.6;
    Spectrogram s = axisFromC2(bpo, frames);
    std::vector<float> hann(NF);
    for (int i = 0; i < NF; i++) {
        hann[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / NF));
    }
    // where each axis bin falls on the spectrum's and the cepstrum's (1/quefrency) axes, for linear interpolation
    std::vector<double> specPos(s.bins), cepsFreq(s.bins);
    for (int b = 0; b < s.bins; b++) {
        cepsFreq[b] = specPos[b] = s.p.fmin * pow(2.0, (double)b / bpo);
    }
    for (double &v : specPos) {
        v = v * NF / SR;
    }
    const int *perm = WDL_fft_permute_tab(NF / 2);
    std::vector<WDL_FFT_REAL> buf(NF), ceps(NF);
    std::vector<float> Z(NF / 2 + 1), C(NF / 2);
    const std::vector<float> gs(Z.size(), (float)(G_SPEC / 2)), gc(C.size(), (float)G_CEPS);
    const int nz = (int)Z.size(), nc = (int)C.size();
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < NF; i++) {
            const long long j = (long long)f * HOP - NF / 2 + i;
            buf[i] = (j >= 0 && j < (long long)x.size() ? x[j] : 0) * hann[i];
        }
        WDL_real_fft(buf.data(), NF, 0);
        const WDL_FFT_COMPLEX *X = (const WDL_FFT_COMPLEX *)buf.data();
        // (|X|/2)^g: numpy's |stft| is half of WDL's; as power^(g/2), vectorised
        Z[0] = 0.25f * X[0].re * X[0].re, Z[NF / 2] = 0.25f * X[0].im * X[0].im;
        for (int k = 1; k < NF / 2; k++) {
            Z[k] = 0.25f * (X[perm[k]].re * X[perm[k]].re + X[perm[k]].im * X[perm[k]].im);
        }
        vvpowf(Z.data(), gs.data(), Z.data(), &nz);
        std::fill(ceps.begin(), ceps.end(), 0);
        WDL_FFT_COMPLEX *Y = (WDL_FFT_COMPLEX *)ceps.data();
        Y[0].re = Z[0], Y[0].im = Z[NF / 2];
        for (int k = 1; k < NF / 2; k++) {
            Y[perm[k]].re = Z[k];
        }
        WDL_real_fft(ceps.data(), NF, 1); // N x numpy's irfft
        for (int q = 0; q < NF / 2; q++) {
            C[q] = std::max(0.0f, (float)(ceps[q] / NF));
        }
        vvpowf(C.data(), gc.data(), C.data(), &nc);
        float *out = &s.mag[(size_t)f * s.bins];
        for (int b = 0; b < s.bins; b++) {
            const double pos = specPos[b];
            const int k = std::min(NF / 2 - 1, (int)pos);
            const double spec = Z[k] + (Z[k + 1] - Z[k]) * (pos - k);
            // cepstrum at quefrency SR/f (q = 1 .. NF/2-1; numpy's interp holds the end values outside)
            const double q = std::max(1.0, std::min(NF / 2.0 - 1, SR / cepsFreq[b]));
            const int qi = std::min(NF / 2 - 2, (int)q);
            // interpolated linearly in frequency, as transcribe does on the reversed 1/q axis
            const double fa = SR / (double)(qi + 1), fb = SR / (double)qi, fq = SR / q;
            const double cv =
                fb > fa ? C[qi + 1] + (C[qi] - C[qi + 1]) * (fq - fa) / (fb - fa) : C[qi];
            out[b] = (float)(spec * cv);
        }
    }
    return s;
}

// Adds (frame, pitch) cells every voter's notes cover but `raw` lacks: a run of them extends a touching
// same-pitch note, else becomes a note (if long enough) with the voters' mean strength.
static void vote(std::vector<NoteEstimate> &raw,
                 const std::vector<std::vector<NoteEstimate>> &voters, int F, int lo, int n,
                 int minLen) {
    if (voters.empty()) {
        return;
    }
    std::vector<unsigned char> mine(F), all(F), cov(F);
    std::vector<float> str(F);
    for (int p = 0; p < n; p++) {
        const int pitch = lo + p;
        std::fill(mine.begin(), mine.end(), 0), std::fill(all.begin(), all.end(), 1),
            std::fill(str.begin(), str.end(), 0.0f);
        for (const NoteEstimate &e : raw) {
            if (e.pitch == pitch) {
                std::fill(mine.begin() + e.f0, mine.begin() + std::min(F, e.f1), 1);
            }
        }
        for (const std::vector<NoteEstimate> &v : voters) {
            std::fill(cov.begin(), cov.end(), 0);
            for (const NoteEstimate &e : v) {
                if (e.pitch == pitch) {
                    for (int f = e.f0; f < std::min(F, e.f1); f++) {
                        cov[f] = 1, str[f] += e.strength / (float)voters.size();
                    }
                }
            }
            for (int f = 0; f < F; f++) {
                all[f] &= cov[f];
            }
        }
        for (int f = 0; f < F;) {
            if (!all[f] || mine[f]) {
                f++;
                continue;
            }
            int b = f;
            while (b < F && all[b] && !mine[b]) {
                b++;
            }
            NoteEstimate *before = nullptr, *after = nullptr;
            for (NoteEstimate &e : raw) {
                if (e.pitch == pitch && e.f1 == f) {
                    before = &e;
                } else if (e.pitch == pitch && e.f0 == b) {
                    after = &e;
                }
            }
            if (before) {
                before->f1 = b;
            } else if (after) {
                after->f0 = f;
            } else if (b - f >= minLen) {
                double sum = 0;
                for (int g = f; g < b; g++) {
                    sum += str[g];
                }
                raw.push_back({f, b, pitch, (float)(sum / (b - f))});
            }
            f = b;
        }
    }
}

std::vector<NoteEstimate> estimateNotes(const Spectrogram &s, const std::vector<float> &mono,
                                        const BasicPitch *bp, const EstimateParams &ep,
                                        const std::function<bool(double)> &progress) {
    std::vector<NoteEstimate> out;
    const int lo = ep.lo, hi = ep.hi, n = hi - lo + 1, F = s.frames;
    if (F <= 0 || n <= 0) {
        return out;
    }
    // progress: main NMF, SACF, the three voters
    const double nmfEnd = ep.vote ? 0.3 : ep.sacfAbs > 0 ? 0.6 : 1, sacfEnd = ep.vote ? 0.45 : 1;
    const double nmfShare = nmfEnd;
    std::atomic<bool> cancelled{false};
    auto stage = [&](double a, double b) {
        return [&, a, b](double f) {
            if (progress && !progress(a + (b - a) * f)) {
                cancelled = true;
            }
            return !cancelled;
        };
    };
    std::vector<float> roll = nmfRoll(s, lo, hi, ep.adapt, stage(0, nmfShare));
    if (cancelled) {
        return out;
    }

    std::vector<unsigned char> gate;
    if (bp && bp->frames > 0) {
        gate.assign((size_t)F * n, 0);
        for (int f = 0; f < F; f++) {
            const int g = std::min(f, bp->frames - 1);
            for (int p = 0; p < n; p++) {
                float m = 0;
                for (int d = -1; d <= 1; d++) {
                    const int midi = lo + p + d;
                    if (midi >= BasicPitch::LOWEST &&
                        midi < BasicPitch::LOWEST + BasicPitch::PITCHES) {
                        m = std::max(m, bp->noteAt(g, midi));
                    }
                }
                gate[(size_t)f * n + p] = m > ep.gate;
            }
        }
    }
    const std::vector<unsigned char> *gp = gate.empty() ? nullptr : &gate;
    Activity act = activity(roll, F, n, gp, ep.abs, ep.rel, ep.oct);
    if (ep.sacfAbs > 0) {
        std::vector<float> sroll = sacfRoll(mono, F, lo, hi, stage(nmfShare, sacfEnd));
        if (cancelled) {
            return out;
        }
        // the SACF fails on different notes than the NMF: add what it finds confidently
        const Activity sa = activity(sroll, F, n, gp, ep.sacfAbs, ep.rel, ep.oct);
        for (size_t i = 0; i < act.on.size(); i++) {
            act.on[i] |= sa.on[i], act.hold[i] |= sa.hold[i];
            act.S[i] =
                std::max(act.S[i], sa.S[i] * (float)(ep.abs / ep.sacfAbs)); // on the NMF's scale
        }
    }

    // runs: start on an `on` frame, continue over `hold` frames bridging short dropouts, split where
    // basic-pitch sees a new onset at that pitch and the note's own level dipped before it (basic-pitch
    // also reports onsets on a held note when another voice stops)
    const double fr = (double)SR / HOP;
    const int minLen = std::max(1, (int)lround(ep.minLength * fr)),
              bridge = (int)lround(ep.bridge * fr);
    auto reattack = [&](int f, int midi) {
        if (!bp || bp->frames <= 0 || midi < BasicPitch::LOWEST ||
            midi >= BasicPitch::LOWEST + BasicPitch::PITCHES) {
            return false;
        }
        const int g = std::min(f, bp->frames - 1);
        const float o = bp->onsetAt(g, midi);
        if (o <= 0.5f) {
            return false;
        }
        for (int d = -2; d <= 2; d++) {
            const int h = std::max(0, std::min(bp->frames - 1, g + d));
            if (d && (bp->onsetAt(h, midi) > o || (d < 0 && bp->onsetAt(h, midi) == o))) {
                return false;
            }
        }
        const int p = midi - lo, w = (int)lround(0.07 * fr);
        float dip = 1e30f, after = 0;
        for (int h = std::max(0, f - w); h < f; h++) {
            dip = std::min(dip, roll[(size_t)h * n + p]);
        }
        for (int h = f; h < std::min(F, f + w); h++) {
            after = std::max(after, roll[(size_t)h * n + p]);
        }
        return dip < REATTACK_DIP * after;
    };
    auto runs = [&](Activity &act) {
        std::vector<NoteEstimate> out;
        for (int p = 0; p < n; p++) {
            for (int f = 0; f < F;) {
                if (!act.on[(size_t)f * n + p]) {
                    f++;
                    continue;
                }
                const int start = f;
                int last = f, g = f + 1;
                bool split = false;
                for (; g < F && g - last <= bridge + 1; g++) {
                    if (!act.hold[(size_t)g * n + p]) {
                        continue;
                    }
                    if (g - start >= minLen && reattack(g, lo + p)) {
                        split = true;
                        break;
                    }
                    last = g;
                }
                const int end = split ? g : last + 1;
                if (end - start >= minLen) {
                    double sum = 0;
                    for (int h = start; h < end; h++) {
                        sum += act.S[(size_t)h * n + p];
                    }
                    out.push_back({start, end, lo + p, (float)(sum / (end - start))});
                }
                f = end;
                if (split) {
                    act.on[(size_t)end * n + p] = 1; // the re-attack starts the next note
                }
            }
        }
        return out;
    };
    out = runs(act);

    if (ep.vote) {
        // the main detector on three other front ends; each misses notes the others find, so only what
        // all of them agree on is added
        struct Voter {
            int bpo;
            double scale;
            bool cfp;
            double abs;
        };
        static const Voter VOTERS[] = {
            {60, 1.0, false, 0.06}, {36, 1.5, false, 0.06}, {36, 1.0, true, 0.15}
        };
        // in parallel: each is about as costly as the main NMF; progress counts finished iterations
        std::vector<std::vector<NoteEstimate>> voters(3);
        std::atomic<int> iterations{0};
        auto voterProgress = [&](double) {
            const double f = std::min(1.0, ++iterations / (3.0 * ITERS));
            if (progress && !progress(sacfEnd + (1 - sacfEnd) * f)) {
                cancelled = true;
            }
            return !cancelled;
        };
        parallel(3, cancelled, [&](int i) {
            const Voter &v = VOTERS[i];
            const Spectrogram fe =
                v.cfp ? computeCFP(mono, v.bpo) : computeCQT(mono, v.bpo, v.scale);
            if (cancelled) {
                return;
            }
            const std::vector<float> vr = nmfRoll(fe, lo, hi, false, voterProgress, v.cfp ? 1 : NH);
            if (cancelled) {
                return;
            }
            Activity va = activity(vr, F, n, gp, v.abs, ep.rel, ep.oct);
            voters[(size_t)i] = runs(va);
        });
        if (cancelled) {
            return {};
        }
        vote(out, voters, F, lo, n, minLen);
    }

    if (ep.octaveDoublings) {
        Partials P(mono, F);
        out = octaveDoublings(out, P, hi, fr);
    }
    std::sort(out.begin(), out.end(), [](const NoteEstimate &a, const NoteEstimate &b) {
        return a.f0 < b.f0 || (a.f0 == b.f0 && a.pitch < b.pitch);
    });
    return out;
}

// --- voices -------------------------------------------------------------------------------------

static const double AGE = 2.0,
                    AGE_CAP =
                        2.0; // semitones of cost per second a strand has been silent, capped (s)
static const double POLYPHONY_SHARE = 0.003;

std::vector<int> assignVoices(const std::vector<NoteEstimate> &notes, double frameRate,
                              int maxVoices) {
    std::vector<int> voice(notes.size(), -1);
    if (notes.empty() || maxVoices < 1) {
        return voice;
    }
    int F = 0;
    for (const NoteEstimate &n : notes) {
        F = std::max(F, n.f1);
    }
    std::vector<int> cnt(F + 1, 0);
    for (const NoteEstimate &n : notes) {
        for (int f = n.f0; f < n.f1; f++) {
            cnt[f]++;
        }
    }
    int sounding = 0;
    for (int c : cnt) {
        sounding += c > 0;
    }
    int nv = 1;
    for (int k = 2; k <= maxVoices; k++) {
        int m = 0;
        for (int c : cnt) {
            m += c >= k;
        }
        if (m >= POLYPHONY_SHARE * sounding) {
            nv = k;
        }
    }
    // resting pitch per strand (top first) before it has sung: spread over the notes' range
    std::vector<int> pitches;
    pitches.reserve(notes.size());
    for (const NoteEstimate &n : notes) {
        pitches.push_back(n.pitch);
    }
    std::sort(pitches.begin(), pitches.end());
    std::vector<double> rest(nv);
    for (int v = 0; v < nv; v++) {
        const double pos = (1 - (v + 0.5) / nv) * (double)(pitches.size() - 1);
        const int i = (int)pos;
        rest[v] = pitches[i] +
                  (i + 1 < (int)pitches.size() ? (pitches[i + 1] - pitches[i]) * (pos - i) : 0);
    }

    std::vector<int> order(notes.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = (int)i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return notes[a].f0 < notes[b].f0; });
    std::vector<int> strandOf(notes.size(), -1), current(nv, -1); // note sounding on each strand
    std::vector<double> last(nv, -1);
    std::vector<int> silentFrom(nv, -1); // frame the strand fell silent, -1 if it never sang
    for (size_t g = 0; g < order.size();) {
        const int t = notes[order[g]].f0;
        std::vector<int> fresh;
        for (; g < order.size() && notes[order[g]].f0 == t; g++) {
            fresh.push_back(order[g]);
        }
        for (int v = 0; v < nv; v++) {
            if (current[v] >= 0 && notes[current[v]].f1 <= t) {
                silentFrom[v] = notes[current[v]].f1, current[v] = -1;
            }
        }
        std::vector<int> free;
        for (int v = 0; v < nv; v++) {
            if (current[v] < 0) {
                free.push_back(v);
            }
        }
        // more new notes than free strands: the strongest get a strand
        std::sort(fresh.begin(), fresh.end(),
                  [&](int a, int b) { return notes[a].strength > notes[b].strength; });
        if (fresh.size() > free.size()) {
            fresh.resize(free.size());
        }
        std::sort(fresh.begin(), fresh.end(),
                  [&](int a, int b) { return notes[a].pitch > notes[b].pitch; });

        // place as many as fit without crossing, at the lowest cost (k = all of them first, then fewer)
        const int nf = (int)fresh.size(), nfree = (int)free.size();
        std::vector<int> best, bestNotes;
        for (int k = nf; k >= 0 && best.empty() && (k > 0 || nf == 0); k--) {
            double bestCost = 1e30;
            for (int nm = 0; nm < (1 << nf); nm++) {
                if (__builtin_popcount(nm) != k) {
                    continue;
                }
                for (int sm = 0; sm < (1 << nfree); sm++) {
                    if (__builtin_popcount(sm) != k) {
                        continue;
                    }
                    std::vector<int> sub, combo;
                    for (int i = 0; i < nf; i++) {
                        if (nm >> i & 1) {
                            sub.push_back(fresh[i]); // high to low
                        }
                    }
                    for (int i = 0; i < nfree; i++) {
                        if (sm >> i & 1) {
                            combo.push_back(free[i]); // top to bottom
                        }
                    }
                    std::vector<int> pitchOn(nv, -1000);
                    for (int v = 0; v < nv; v++) {
                        if (current[v] >= 0) {
                            pitchOn[v] = notes[current[v]].pitch;
                        }
                    }
                    for (int i = 0; i < k; i++) {
                        pitchOn[combo[i]] = notes[sub[i]].pitch;
                    }
                    bool crossing = false;
                    int above = 1000;
                    for (int v = 0; v < nv; v++) {
                        if (pitchOn[v] > -1000) {
                            crossing |= pitchOn[v] > above;
                            above = pitchOn[v];
                        }
                    }
                    if (crossing) {
                        continue;
                    }
                    double cost = 0;
                    for (int i = 0; i < k; i++) {
                        const int v = combo[i];
                        const double silent =
                            silentFrom[v] < 0 ? AGE_CAP
                                              : std::min(AGE_CAP, (t - silentFrom[v]) / frameRate);
                        cost += fabs(notes[sub[i]].pitch - (last[v] >= 0 ? last[v] : rest[v])) +
                                AGE * silent;
                    }
                    if (cost < bestCost) {
                        bestCost = cost, best = combo, bestNotes = sub;
                    }
                }
            }
        }
        for (size_t i = 0; i < best.size(); i++) {
            const int v = best[i], n = bestNotes[i];
            strandOf[n] = v, current[v] = n, last[v] = notes[n].pitch;
        }
    }

    // the strand singing most is the lead
    std::vector<long long> sung(nv, 0);
    for (size_t i = 0; i < notes.size(); i++) {
        if (strandOf[i] >= 0) {
            sung[strandOf[i]] += notes[i].f1 - notes[i].f0;
        }
    }
    const int lead = (int)(std::max_element(sung.begin(), sung.end()) - sung.begin());
    std::vector<int> voiceOf(nv);
    for (int v = 0, next = 1; v < nv; v++) {
        voiceOf[v] = v == lead ? 0 : next++;
    }
    for (size_t i = 0; i < notes.size(); i++) {
        voice[i] = strandOf[i] >= 0 ? voiceOf[strandOf[i]] : -1;
    }
    return voice;
}
