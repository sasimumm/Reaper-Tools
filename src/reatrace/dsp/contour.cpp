#include "dsp/contour.h"

#include <algorithm>
#include <cmath>

static const int HARMONICS = 8;
static const double HARMONIC_DECAY = 0.85; // weight of harmonic h is DECAY^(h-1)
static const int CANDIDATES = 5;
static const double MIN_HARMONICITY = 0.3; // normalised harmonic score needed to count as voiced
static const double LEVEL_RANGE_DB = 40;   // voiced frames are within this of the loud (p90) frames
static const double JUMP_COST = 0.3; // Viterbi cost per semitone of pitch change between frames

static double pitchOfBin(const Spectrogram &s, double bin) {
    return 69 + 12 * log2(s.p.fmin / 440) + 12 * bin / s.p.binsPerOctave;
}

static double median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

Contour trackPitch(const Spectrogram &s, double lowPitch, double highPitch) {
    Contour out;
    out.pitch.assign(s.frames, -1);
    out.strength.assign(s.frames, 0);
    const int bpo = s.p.binsPerOctave;
    const int k0 = std::max(0, (int)ceil(s.binOfPitch(lowPitch))),
              k1 = std::min(s.bins - 2, (int)floor(s.binOfPitch(highPitch)));
    if (s.frames == 0 || k0 >= k1) {
        return out;
    }

    double off[HARMONICS + 1], w[HARMONICS + 1], wsum = 0;
    for (int h = 1; h <= HARMONICS; h++) {
        off[h] = bpo * log2((double)h), w[h] = pow(HARMONIC_DECAY, h - 1), wsum += w[h];
    }

    struct Cand {
        double pitch, score;
    };
    std::vector<std::vector<Cand>> cands(s.frames);
    std::vector<double> level(s.frames);
    std::vector<float> c(s.bins), score(s.bins);
    for (int f = 0; f < s.frames; f++) {
        float peak = 0;
        for (int b = 0; b < s.bins; b++) {
            c[b] = sqrtf(s.at(f, b)), peak = b >= k0 ? std::max(peak, c[b]) : peak;
        }
        level[f] = 40 * log10(std::max(peak, 1e-6f)); // c = sqrt(mag), so 40 log10 = dB of mag
        if (peak <= 0) {
            continue;
        }
        for (int k = k0; k <= k1; k++) {
            double sum = 0;
            for (int h = 1; h <= HARMONICS; h++) {
                const double b = k + off[h];
                const int i = (int)b;
                if (i + 1 >= s.bins) {
                    break;
                }
                sum += w[h] * (c[i] + (c[i + 1] - c[i]) * (b - i));
            }
            score[k] = (float)(sum / (wsum * peak));
        }
        std::vector<Cand> &cf = cands[f];
        for (int k = k0 + 1; k < k1; k++) {
            if (score[k] >= score[k - 1] && score[k] > score[k + 1]) {
                // parabolic interpolation for a fractional bin
                const double a = score[k - 1], b = score[k], d = score[k + 1], den = a - 2 * b + d;
                const double dk = den < 0 ? 0.5 * (a - d) / den : 0;
                cf.push_back({pitchOfBin(s, k + dk), b});
            }
        }
        std::sort(cf.begin(), cf.end(),
                  [](const Cand &a, const Cand &b) { return a.score > b.score; });
        if (cf.size() > CANDIDATES) {
            cf.resize(CANDIDATES);
        }
    }

    std::vector<double> lv = level;
    std::nth_element(lv.begin(), lv.begin() + lv.size() * 9 / 10, lv.end());
    const double minLevel = lv[lv.size() * 9 / 10] - LEVEL_RANGE_DB;
    std::vector<bool> voiced(s.frames);
    for (int f = 0; f < s.frames; f++) {
        voiced[f] =
            !cands[f].empty() && cands[f][0].score >= MIN_HARMONICITY && level[f] >= minLevel;
    }

    // Viterbi over each voiced run
    for (int a = 0; a < s.frames;) {
        if (!voiced[a]) {
            a++;
            continue;
        }
        int b = a;
        while (b + 1 < s.frames && voiced[b + 1]) {
            b++;
        }
        std::vector<std::vector<double>> cost(b - a + 1);
        std::vector<std::vector<int>> from(b - a + 1);
        for (int f = a; f <= b; f++) {
            const std::vector<Cand> &cf = cands[f];
            cost[f - a].assign(cf.size(), 0), from[f - a].assign(cf.size(), -1);
            for (size_t j = 0; j < cf.size(); j++) {
                const double emit = -log(cf[j].score + 1e-3);
                if (f == a) {
                    cost[0][j] = emit;
                    continue;
                }
                const std::vector<Cand> &cp = cands[f - 1];
                double best = 1e30;
                for (size_t i = 0; i < cp.size(); i++) {
                    const double v =
                        cost[f - a - 1][i] + JUMP_COST * fabs(cf[j].pitch - cp[i].pitch);
                    if (v < best) {
                        best = v, from[f - a][j] = (int)i;
                    }
                }
                cost[f - a][j] = best + emit;
            }
        }
        int j =
            (int)(std::min_element(cost.back().begin(), cost.back().end()) - cost.back().begin());
        for (int f = b; f >= a; f--) {
            out.pitch[f] = (float)cands[f][j].pitch, out.strength[f] = (float)cands[f][j].score;
            j = from[f - a][j];
        }
        a = b + 1;
    }
    return out;
}

bool contourMedian(const Contour &c, int f0, int f1, int skip, double *pitch) {
    f0 = std::max(0, f0), f1 = std::min((int)c.pitch.size() - 1, f1);
    if (f1 < f0) {
        return false;
    }
    std::vector<double> all, late;
    for (int f = f0; f <= f1; f++) {
        if (c.pitch[f] >= 0) {
            all.push_back(c.pitch[f]);
            if (f >= f0 + skip) {
                late.push_back(c.pitch[f]);
            }
        }
    }
    if (all.size() * 2 < (size_t)f1 - (size_t)f0 + 1) {
        return false; // f1 >= f0 here
    }
    *pitch = median(late.size() * 2 >= all.size() ? late : all);
    return true;
}

std::vector<Segment> segmentContour(const Contour &c, double frameRate,
                                    const std::vector<int> *onsets, double minDuration,
                                    double tolerance, double skipStart) {
    const int n = (int)c.pitch.size(),
              half = std::max(1, (int)lround(0.075 * frameRate)); // ~150 ms window
    // vibrato-smoothed pitch: mean of voiced frames in the window, within the same voiced run
    std::vector<double> smooth(n, -1);
    for (int f = 0; f < n; f++) {
        if (c.pitch[f] < 0) {
            continue;
        }
        double sum = 0;
        int cnt = 0;
        for (int g = f; g >= std::max(0, f - half) && c.pitch[g] >= 0; g--) {
            sum += c.pitch[g], cnt++;
        }
        for (int g = f + 1; g <= std::min(n - 1, f + half) && c.pitch[g] >= 0; g++) {
            sum += c.pitch[g], cnt++;
        }
        smooth[f] = sum / cnt;
    }

    const int minFrames = std::max(1, (int)lround(minDuration * frameRate)),
              skip = (int)lround(skipStart * frameRate);
    std::vector<Segment> segs;
    auto close = [&](int a, int b) {
        if (b - a + 1 < minFrames) {
            return;
        }
        double p = 0;
        if (contourMedian(c, a, b, std::min(skip, (b - a + 1) / 3), &p)) {
            segs.push_back({a, b, p});
        }
    };
    int start = -1;
    double sum = 0;
    for (int f = 0; f <= n; f++) {
        const bool v = f < n && smooth[f] >= 0;
        if (start >= 0 && (!v || fabs(smooth[f] - sum / (f - start)) > tolerance)) {
            close(start, f - 1);
            start = -1;
        }
        if (v && start < 0) {
            start = f, sum = 0;
        }
        if (v) {
            sum += smooth[f];
        }
    }

    // join pieces of the same note split by a short dropout
    std::vector<Segment> merged;
    for (const Segment &sg : segs) {
        if (!merged.empty() && sg.f0 - merged.back().f1 <= 3 &&
            lround(sg.pitch) == lround(merged.back().pitch)) {
            merged.back().f1 = sg.f1;
            contourMedian(c, merged.back().f0, sg.f1, skip, &merged.back().pitch);
        } else {
            merged.push_back(sg);
        }
    }

    // legato note changes: move both edges to where the raw pitch crosses halfway between the notes
    for (size_t i = 0; i + 1 < merged.size(); i++) {
        Segment &a = merged[i], &b = merged[i + 1];
        bool voicedGap = true;
        for (int f = a.f1; f <= b.f0 && voicedGap; f++) {
            voicedGap = c.pitch[f] >= 0;
        }
        if (!voicedGap || b.f0 - a.f1 > (int)lround(0.3 * frameRate)) {
            continue;
        }
        const double mid = 0.5 * (a.pitch + b.pitch);
        int cross = b.f0;
        for (int f = a.f1; f <= b.f0; f++) {
            if ((c.pitch[f] - mid) * (b.pitch - a.pitch) >= 0) {
                cross = f;
                break;
            }
        }
        a.f1 = std::max(a.f0, cross - 1), b.f0 = cross;
    }
    // starts after silence: snap to a nearby onset
    if (onsets) {
        for (size_t i = 0; i < merged.size(); i++) {
            Segment &sg = merged[i];
            const int lo = i > 0 ? merged[i - 1].f1 + 1 : 0, range = (int)lround(0.06 * frameRate);
            int best = -1;
            for (int f : *onsets) {
                if (f >= lo && f < sg.f1 && abs(f - sg.f0) <= range &&
                    (best < 0 || abs(f - sg.f0) < abs(best - sg.f0))) {
                    best = f;
                }
            }
            if (best >= 0) {
                sg.f0 = best;
            }
        }
    }
    return merged;
}

std::vector<int> detectOnsets(const Spectrogram &s) {
    std::vector<double> flux(s.frames, 0);
    std::vector<float> prev(s.bins), cur(s.bins);
    for (int f = 0; f < s.frames; f++) {
        double sum = 0;
        for (int b = 0; b < s.bins; b++) {
            cur[b] = log1pf(100.0f * s.at(f, b));
            sum += std::max(0.0f, cur[b] - prev[b]);
        }
        if (f > 0) {
            flux[f] = sum;
        }
        prev.swap(cur);
    }
    double mean = 0, var = 0;
    for (double v : flux) {
        mean += v / std::max(1, s.frames);
    }
    for (double v : flux) {
        var += (v - mean) * (v - mean) / std::max(1, s.frames);
    }
    const double thr = mean + 1.5 * sqrt(var);
    const int around = 3, minGap = std::max(1, (int)lround(0.05 * s.frameRate()));
    std::vector<int> out;
    for (int f = 1; f < s.frames; f++) {
        if (flux[f] <= thr) {
            continue;
        }
        bool peak = true;
        for (int g = std::max(0, f - around); g <= std::min(s.frames - 1, f + around) && peak;
             g++) {
            peak = g == f || flux[g] < flux[f] || (flux[g] == flux[f] && g > f);
        }
        if (peak && (out.empty() || f - out.back() >= minGap)) {
            out.push_back(f);
        }
    }
    return out;
}
