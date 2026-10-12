// Self-check for the NSGT spectrogram: run `build/nsgt_test`.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dsp/nsgt.h"
#include "index.h"

static Spectrogram analyze(const std::vector<float> &x) {
    return computeNSGT(
        NSGTParams(), static_cast<long long>(x.size()), [&](long long start, int n, float *out) {
            for (int i = 0; i < n; i++) {
                long long j = start + i;
                out[i] = j >= 0 && j < static_cast<long long>(x.size()) ? x[idx(j)] : 0;
            }
        });
}

int main() {
    const double sr = NSGTParams().sampleRate;

    // A4 sine at -6 dBFS: the A4 bin reads 0.5 everywhere away from the edges, and is the loudest bin.
    {
        std::vector<float> x(static_cast<size_t>(10 * sr));
        for (size_t i = 0; i < x.size(); i++) {
            x[i] = 0.5f * static_cast<float>(sin(2 * M_PI * 440 * static_cast<double>(i) / sr));
        }
        Spectrogram s = analyze(x);
        const int a4 = static_cast<int>(lround(s.binOfPitch(69)));
        double lo = 1e9, hi = 0;
        for (int f = 20; f < s.frames - 20; f++) {
            lo = fmin(lo, s.at(f, a4));
            hi = fmax(hi, s.at(f, a4));
            for (int b = 0; b < s.bins; b++) {
                assert(static_cast<double>(s.at(f, b)) <= static_cast<double>(s.at(f, a4)) + 1e-6);
            }
        }
        printf("sine: bins=%d frames=%d A4 bin=%d magnitude %.4f..%.4f\n", s.bins, s.frames, a4, lo,
               hi);
        assert(lo > 0.48 && hi < 0.52);
    }

    // Clicks, including ones near block boundaries: a high bin peaks at the click's frame.
    {
        std::vector<float> x(static_cast<size_t>(6 * sr));
        const long long clicks[] = {12345, 25000, 28160, 28300, 56320, 77777, 100000};
        for (long long c : clicks) {
            x[idx(c)] = 1;
        }
        Spectrogram s = analyze(x);
        const int b = static_cast<int>(lround(s.binOfPitch(96))); // C7, ~2 kHz: short kernel
        for (long long c : clicks) {
            int expect = static_cast<int>(lround(static_cast<double>(c) / s.p.hop)),
                best = expect - 8;
            for (int f = expect - 8; f <= expect + 8; f++) {
                if (s.at(f, b) > s.at(best, b)) {
                    best = f;
                }
            }
            printf("click at %lld: expected frame %d, peak at %d\n", c, expect, best);
            assert(abs(best - expect) <= 1);
        }
    }

    // Nonsense parameters (from user settings) are clamped instead of breaking the indexing.
    {
        NSGTParams bad;
        bad.fmin = 0;
        bad.fmax = -1;
        bad.hop = 1000;
        bad.binsPerOctave = 100000;
        bad.gamma = -5;
        bad.sampleRate = 1;
        std::vector<float> x(50000, 0.1f);
        Spectrogram s =
            computeNSGT(bad, static_cast<long long>(x.size()), [&](long long, int n, float *out) {
                for (int i = 0; i < n; i++) {
                    out[i] = 0.1f;
                }
            });
        assert(s.bins > 0 && s.frames > 0 && s.mag.size() == idx(s.bins) * idx(s.frames));
        printf("bad params: bins=%d frames=%d hop=%d\n", s.bins, s.frames, s.p.hop);
    }

    // Speed on three minutes of noise; the fast display values match cellValue() within one step.
    {
        std::vector<float> x(static_cast<size_t>(180 * sr));
        for (float &v : x) {
            v = static_cast<float>(rand() / static_cast<double>(RAND_MAX)) - 0.5f;
        }
        auto t0 = std::chrono::steady_clock::now();
        Spectrogram s = analyze(x);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
        printf("180 s of audio: %.0f ms, %.1f MB\n", ms,
               static_cast<double>(s.mag.size()) * 4 / 1e6);

        RenderParams rp;
        autoCalibrate(s, rp);
        std::vector<unsigned char> val;
        for (int mode = 0; mode < 3; mode++) {
            rp.peaksOnly = mode >= 1;
            rp.subtractNoise = mode >= 2;
            t0 = std::chrono::steady_clock::now();
            renderValues(s, rp, val);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                     .count();
            int worst = 0;
            for (int f = 0; f < s.frames; f += 7) {
                for (int b = 0; b < s.bins; b++) {
                    worst = std::max(worst,
                                     abs(val[idx(f, s.bins, b)] -
                                         static_cast<int>(lround(cellValue(s, rp, f, b) * 255))));
                }
            }
            printf("display values (peaks %d, noise %d): %.0f ms, max difference %d\n",
                   rp.peaksOnly, rp.subtractNoise, ms, worst);
            assert(worst <= 1);
        }
    }
    printf("ok\n");
}
