#include "dsp/nsgt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>

#include "dsp/wdlfft.h"
#include "index.h"

// WDL's FFT tops out at 32768 points, so the signal is processed in overlapping blocks
// (sliCQ-style). Each block gets one forward FFT; every bin then multiplies its Gaussian
// window into the spectrum and runs a small inverse FFT of just the window's support,
// which yields that bin's magnitude decimated to the hop.
static const int BLOCK = 32768;

static int nextPow2(int n) {
    int p = 1;
    while (p < n) {
        p *= 2;
    }
    return p;
}

double Spectrogram::binOfPitch(double midiPitch) const {
    double f = 440.0 * pow(2.0, (midiPitch - 69) / 12.0);
    return p.binsPerOctave * log2(f / p.fmin);
}

// Parameters come from user settings: keep them in the range the indexing below assumes.
static NSGTParams sanitize(NSGTParams p) {
    p.sampleRate = std::max(8000.0, std::min(96000.0, p.sampleRate));
    p.binsPerOctave = std::max(12, std::min(120, p.binsPerOctave));
    p.hop = std::max(
        64,
        std::min(BLOCK / 8,
                 nextPow2(std::max(1, std::min(p.hop, BLOCK))))); // power of two dividing the block
    p.fmin = std::max(10.0, std::min(p.sampleRate * 0.25, p.fmin));
    p.fmax = std::max(p.fmin * 2, std::min(p.sampleRate * 0.45, p.fmax));
    p.gamma = std::max(0.0, std::min(1000.0, p.gamma));
    return p;
}

void readPadded(std::span<const float> x, long long start, std::span<float> out) {
    for (size_t i = 0; i < out.size(); i++) {
        const long long j = start + static_cast<long long>(i);
        out[i] = j >= 0 && j < static_cast<long long>(x.size()) ? x[idx(j)] : 0;
    }
}

static Spectrogram computeOne(const NSGTParams &p, long long numSamples, const SampleReader &read,
                              const std::function<bool(double)> &progress) {
    numSamples = std::max(
        0LL, std::min(numSamples, static_cast<long long>(INT_MAX) * 64)); // frames fit in an int
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
    s.bins = std::max(1, static_cast<int>(ceil(p.binsPerOctave * log2(fmax / p.fmin))));
    s.frames = static_cast<int>(numSamples / hop) + 1;
    s.mag.assign(idx(s.frames) * idx(s.bins), 0);

    struct Bin {
        int k0 = 0, k1 = 0, M = 0, step = 0;
        std::vector<float> w;
    };
    std::vector<Bin> bins(idx(s.bins));
    double maxSigmaT = 0;
    for (int b = 0; b < s.bins; b++) {
        const double f = p.fmin * pow(2.0, static_cast<double>(b) / p.binsPerOctave);
        const double sigmaF = std::max((f / Q + p.gamma) / (2 * sqrt(2 * log(2.0))), minSigmaF);
        maxSigmaT = std::max(maxSigmaT, 1 / (2 * M_PI * sigmaF));
        const int r = std::max(1, static_cast<int>(ceil(3 * sigmaF / freqRes))),
                  c = static_cast<int>(lround(f / freqRes));
        Bin &bn = bins[idx(b)];
        bn.k0 = std::max(1, c - r);
        bn.k1 = std::min(N / 2 - 1, c + r);
        bn.M = std::max(nextPow2(bn.k1 - bn.k0 + 1), N / hop);
        bn.step = bn.M * hop / N;
        for (int k = bn.k0; k <= bn.k1; k++) {
            double d = k * freqRes - f;
            bn.w.push_back(static_cast<float>(exp(-d * d / (2 * sigmaF * sigmaF))));
        }
    }

    // Frames closer than `margin` to a block edge would see circular wrap-around; only keep the middle.
    const int margin = std::min(N / 4, static_cast<int>(ceil(3 * maxSigmaT * sr / hop)) * hop);
    const int valid = N - 2 * margin, framesPerBlock = valid / hop;

    std::vector<WDL_FFT_REAL> block(N);
    int maxM = 0;
    for (const Bin &bn : bins) {
        maxM = std::max(maxM, bn.M);
    }
    std::vector<WDL_FFT_COMPLEX> small(idx(maxM));
    const std::span<const int> permN = permuteTab(N / 2);
    // Y = 2*X*w for the analytic signal and |ifft|/N; WDL's real FFT output is already 2x a plain DFT, so 2/N -> 1/N.
    const float scale = 1.0f / N;

    for (int first = 0; first < s.frames; first += framesPerBlock) {
        const long long start = static_cast<long long>(first) * hop - margin;
        read(start, block);
        WDL_real_fft(block.data(), N, 0);
        // WDL's real FFT leaves N/2 complex values in place of the N reals.
#pragma clang unsafe_buffer_usage begin
        const std::span<const WDL_FFT_COMPLEX> X(
            reinterpret_cast<const WDL_FFT_COMPLEX *>(block.data()), idx(N / 2));
#pragma clang unsafe_buffer_usage end
        const int nf = std::min(framesPerBlock, s.frames - first);

        for (int b = 0; b < s.bins; b++) {
            const Bin &bn = bins[idx(b)];
            const std::span<const int> permM = permuteTab(bn.M);
            std::fill_n(small.begin(), bn.M, WDL_FFT_COMPLEX{});
            // Shifting the band down to bin 0 only changes phase, not magnitude.
            for (int k = bn.k0; k <= bn.k1; k++) {
                const WDL_FFT_COMPLEX &x = X[idx(permN[idx(k)])];
                WDL_FFT_COMPLEX &y = small[idx(permM[idx(k - bn.k0)])];
                float w = bn.w[idx(k - bn.k0)] * scale;
                y.re = x.re * w;
                y.im = x.im * w;
            }
            WDL_fft(small.data(), bn.M, 1);
            for (int i = 0; i < nf; i++) {
                const WDL_FFT_COMPLEX &z = small[idx(margin / hop + i) * idx(bn.step)];
                s.mag[idx(first + i, s.bins, b)] = hypotf(z.re, z.im);
            }
        }
        if (progress && !progress(static_cast<double>(first + nf) / s.frames)) {
            return s;
        }
    }

    return s;
}

Spectrogram computeNSGT(const NSGTParams &params, long long numSamples, const SampleReader &read,
                        const std::function<bool(double)> &progress) {
    const NSGTParams p = sanitize(params);
    Spectrogram s = computeOne(p, numSamples, read, progress);
    estimateNoise(s, 0.1);
    return s;
}

void estimateNoise(Spectrogram &s, double fraction) {
    std::vector<std::pair<double, int>> energy(idx(s.frames));
    for (int f = 0; f < s.frames; f++) {
        double e = 0;
        for (int b = 0; b < s.bins; b++) {
            e += static_cast<double>(s.at(f, b)) * static_cast<double>(s.at(f, b));
        }
        energy[idx(f)] = {e, f};
    }
    s.noise.assign(idx(s.bins), 0);
    if (!s.frames) {
        return;
    }
    const int quiet = std::max(1, std::min(s.frames, static_cast<int>(s.frames * fraction)));
    std::nth_element(energy.begin(), energy.begin() + quiet - 1, energy.end());
    for (int i = 0; i < quiet; i++) {
        for (int b = 0; b < s.bins; b++) {
            s.noise[idx(b)] += s.at(energy[idx(i)].second, b) / static_cast<float>(quiet);
        }
    }
}

void autoCalibrate(const Spectrogram &s, RenderParams &rp, int f0, int f1) {
    if (f1 < 0 || f1 > s.frames) {
        f1 = s.frames;
    }
    f0 = std::max(0, std::min(f0, f1));
    std::vector<float> db;
    const size_t a = idx(f0) * idx(s.bins), n = idx(f1) * idx(s.bins),
                 stride = std::max<size_t>(1, (n - a) / 200000);
    for (size_t i = a; i < n; i += stride) {
        db.push_back(20 * log10f(std::max(s.mag[i], 1e-10f)));
    }
    if (db.empty()) {
        return;
    }
    auto pct = [&](double q) {
        auto it = db.begin() +
                  static_cast<std::ptrdiff_t>(std::min(
                      db.size() - 1, static_cast<size_t>(q * static_cast<double>(db.size()))));
        std::nth_element(db.begin(), it, db.end());
        return static_cast<double>(*it);
    };
    const double p30 = pct(0.30), p95 = pct(0.95);
    rp.dbFloor = std::max(std::min(p30, -20.0), -120.0);
    const double np95 = (p95 - rp.dbFloor) / -rp.dbFloor;
    rp.contrast = np95 > 0.01 ? std::max(0.1, std::min(3.0, 0.95 / np95)) : 1;
    rp.brightness = -0.10;
}

static float displayMag(const Spectrogram &s, const RenderParams &rp, int f, int b) {
    float m = s.at(f, b);
    return rp.subtractNoise
               ? std::max(0.0f, m - static_cast<float>(rp.noiseAlpha) * s.noise[idx(b)])
               : m;
}

double cellValue(const Spectrogram &s, const RenderParams &rp, int f, int b) {
    const float m = displayMag(s, rp, f, b);
    const double db = std::max(20.0 * static_cast<double>(log10(std::max(m, 1e-10f))), rp.dbFloor);
    double v = ((db - rp.dbFloor) / -rp.dbFloor + rp.brightness) * rp.contrast;
    v = std::max(0.0, std::min(1.0, v));
    if (rp.peaksOnly && b > 0 && b + 1 < s.bins &&
        !(m > displayMag(s, rp, f, b - 1) && m > displayMag(s, rp, f, b + 1))) {
        v *= rp.peakDim;
    }
    return v;
}

// Maps a float's exponent and top 8 mantissa bits (~0.03 dB steps) straight to the display value.
static const int LUT_SHIFT = 15;

void renderValues(const Spectrogram &s, const RenderParams &rp, std::vector<unsigned char> &out) {
    static std::vector<unsigned char> lut(1 << (32 - LUT_SHIFT - 1));
    for (size_t i = 0; i < lut.size(); i++) {
        unsigned int bits =
            static_cast<unsigned int>(i << LUT_SHIFT) | (1u << (LUT_SHIFT - 1)); // bucket centre
        const float m = std::bit_cast<float>(bits);
        const double db =
            std::max(20.0 * static_cast<double>(log10(std::max(m, 1e-10f))), rp.dbFloor);
        double v = ((db - rp.dbFloor) / -rp.dbFloor + rp.brightness) * rp.contrast;
        lut[i] = static_cast<unsigned char>(lround(std::max(0.0, std::min(1.0, v)) * 255));
    }
    auto value = [&](float m) {
        const unsigned int bits = std::bit_cast<unsigned int>(m);
        return lut[(bits & 0x7fffffff) >> LUT_SHIFT];
    };

    out.resize(s.mag.size());
    std::vector<float> row(idx(s.bins));
    const float alpha = static_cast<float>(rp.noiseAlpha);
    const int dim = static_cast<int>(lround(std::max(0.0, std::min(1.0, rp.peakDim)) * 256));
    for (int f = 0; f < s.frames; f++) {
        const std::span<const float> m =
            std::span(s.mag).subspan(idx(f) * idx(s.bins), idx(s.bins));
        if (rp.subtractNoise) {
            for (int b = 0; b < s.bins; b++) {
                row[idx(b)] = std::max(0.0f, m[idx(b)] - alpha * s.noise[idx(b)]);
            }
        } else {
            std::copy(m.begin(), m.end(), row.begin());
        }
        const std::span<unsigned char> o =
            std::span(out).subspan(idx(f) * idx(s.bins), idx(s.bins));
        for (int b = 0; b < s.bins; b++) {
            int v = value(row[idx(b)]);
            if (rp.peaksOnly && b > 0 && b + 1 < s.bins &&
                !(row[idx(b)] > row[idx(b - 1)] && row[idx(b)] > row[idx(b + 1)])) {
                v = (v * dim) >> 8;
            }
            o[idx(b)] = static_cast<unsigned char>(v);
        }
    }
}

// Graphite (default): a neutral grey ramp with a slight blue bias, so the voices' colours stay
// readable over it. Magma: the perceptual colormap, brighter but orange and yellow at the top.
void makeColormap(double bias, std::span<unsigned int, 256> lut, int colormap) {
    static const std::array<std::array<float, 3>, 9> graphite = {{
        {0.055f, 0.059f, 0.078f},
        {0.098f, 0.106f, 0.141f},
        {0.150f, 0.163f, 0.216f},
        {0.230f, 0.248f, 0.318f},
        {0.352f, 0.375f, 0.460f},
        {0.500f, 0.528f, 0.620f},
        {0.651f, 0.678f, 0.771f},
        {0.790f, 0.812f, 0.885f},
        {0.933f, 0.941f, 0.973f},
    }};
    static const std::array<std::array<float, 3>, 9> magma = {{
        {0.001f, 0.000f, 0.014f},
        {0.090f, 0.048f, 0.228f},
        {0.232f, 0.060f, 0.438f},
        {0.390f, 0.100f, 0.502f},
        {0.550f, 0.161f, 0.506f},
        {0.716f, 0.214f, 0.475f},
        {0.868f, 0.340f, 0.443f},
        {0.967f, 0.534f, 0.498f},
        {0.987f, 0.991f, 0.750f},
    }};
    const auto &ramp = colormap == COLORMAP_MAGMA ? magma : graphite;
    const double exponent = pow(2.0, -bias);
    for (int i = 0; i < 256; i++) {
        const double t = pow(i / 255.0, exponent) * 8;
        const int k = std::min(7, static_cast<int>(t));
        const double fr = t - k;
        unsigned int c = 0;
        for (int ch = 0; ch < 3; ch++) {
            double v = static_cast<double>(ramp[idx(k)][idx(ch)]) +
                       static_cast<double>(ramp[idx(k + 1)][idx(ch)] - ramp[idx(k)][idx(ch)]) * fr;
            c = (c << 8) | static_cast<unsigned int>(std::max(0.0, std::min(255.0, v * 255)));
        }
        lut[idx(i)] = c;
    }
}
