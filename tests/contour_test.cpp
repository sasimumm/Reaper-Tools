// Self-check for vocal analysis: run `build/contour_test`.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dsp/contour.h"
#include "index.h"

// A sung-like tone: 12 harmonics with a "formant" making the 3rd louder than the fundamental.
static void tone(std::vector<float> &x, double sr, double t0, double t1, double pitch,
                 double vibratoCents) {
    double phase[13] = {0};
    for (long long i = static_cast<long long>(t0 * sr); i < static_cast<long long>(t1 * sr); i++) {
        const double t = static_cast<double>(i) / sr;
        const double p = pitch + vibratoCents / 100 * sin(2 * M_PI * 5.5 * t);
        const double f0 = 440 * pow(2.0, (p - 69) / 12);
        const double env = std::min(1.0, std::min(t - t0, t1 - t) / 0.01); // 10 ms fades
        double v = 0;
        for (int h = 1; h <= 12; h++) {
            phase[h] += 2 * M_PI * f0 * h / sr;
            v += (h == 3 ? 2.0 : 1.0 / h) * sin(phase[h]);
        }
        x[idx(i)] += static_cast<float>(0.1 * env * v);
    }
}

int main() {
    const double sr = NSGTParams().sampleRate;
    std::vector<float> x(static_cast<size_t>(2.4 * sr));
    for (float &v : x) {
        v = 0.0005f * (static_cast<float>(rand() / static_cast<double>(RAND_MAX)) - 0.5f);
    }
    tone(x, sr, 0.2, 0.7, 57, 0);   // A3
    tone(x, sr, 0.7, 1.3, 60, 40);  // C4 with +-40 cent vibrato, legato from A3
    tone(x, sr, 1.6, 2.2, 64.3, 0); // E4, 30 cents sharp, after a rest

    Spectrogram s = computeNSGT(
        NSGTParams(), static_cast<long long>(x.size()), [&](long long st, int n, float *o) {
            for (int i = 0; i < n; i++) {
                const long long j = st + i;
                o[i] = j >= 0 && j < static_cast<long long>(x.size()) ? x[idx(j)] : 0;
            }
        });
    const double fr = s.frameRate();
    Contour c = trackPitch(s);

    // contour follows each note (no octave errors despite the loud 3rd harmonic), silent in the rest
    struct {
        double a, b, pitch;
    } notes[] = {{0.3, 0.6, 57}, {0.8, 1.2, 60}, {1.7, 2.1, 64.3}};
    for (auto &n : notes) {
        double p = 0;
        assert(contourMedian(c, static_cast<int>(n.a * fr), static_cast<int>(n.b * fr), 0, &p));
        printf("note %.2f-%.2f s: median %.2f (expected %.2f)\n", n.a, n.b, p, n.pitch);
        assert(fabs(p - n.pitch) < 0.2);
    }
    // the contour doesn't depend on the bins-per-semitone setting
    for (int bps : {1, 2, 10}) {
        NSGTParams np;
        np.binsPerOctave = 12 * bps;
        const Spectrogram sb =
            computeNSGT(np, static_cast<long long>(x.size()), [&](long long st, int n, float *o) {
                for (int i = 0; i < n; i++) {
                    o[i] = st + i >= 0 && st + i < static_cast<long long>(x.size()) ? x[idx(st + i)]
                                                                                    : 0;
                }
            });
        const Contour cb = trackPitch(sb);
        for (auto &n : notes) {
            double p = 0;
            assert(
                contourMedian(cb, static_cast<int>(n.a * fr), static_cast<int>(n.b * fr), 0, &p));
            printf("%d bins/semitone, note %.2f-%.2f s: median %.2f\n", bps, n.a, n.b, p);
            assert(fabs(p - n.pitch) < 0.3);
        }
    }
    int voicedInRest = 0;
    for (int f = static_cast<int>(1.38 * fr); f < static_cast<int>(1.52 * fr); f++) {
        voicedInRest += c.pitch[idx(f)] >= 0;
    }
    printf("voiced frames in the rest: %d\n", voicedInRest);
    assert(voicedInRest == 0);

    // segments: one per note, vibrato averaged out, boundaries near the note changes
    std::vector<int> on = detectOnsets(s);
    std::vector<Segment> segs = segmentContour(c, fr, &on);
    for (const Segment &sg : segs) {
        printf("segment %.3f-%.3f s pitch %.2f\n", sg.f0 / fr, sg.f1 / fr, sg.pitch);
    }
    assert(segs.size() == 3);
    for (int i = 0; i < 3; i++) {
        assert(lround(segs[idx(i)].pitch) == lround(notes[idx(i)].pitch));
    }
    assert(fabs(segs[0].f1 / fr - 0.7) < 0.03 && fabs(segs[1].f0 / fr - 0.7) < 0.03);
    assert(fabs(segs[0].f0 / fr - 0.2) < 0.03 && fabs(segs[2].f0 / fr - 1.6) < 0.03);

    // onsets at the starts after silence
    for (int f : on) {
        printf("onset %.3f s\n", f / fr);
    }
    auto near = [&](double t) {
        for (int f : on) {
            if (fabs(f / fr - t) < 0.03) {
                return true;
            }
        }
        return false;
    };
    assert(near(0.2) && near(1.6));
    printf("ok\n");
}
