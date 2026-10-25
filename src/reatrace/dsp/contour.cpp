// Vocal analysis on the spectrogram: harmonic-sum pitch tracking, segmenting the contour into
// stable-pitch notes, and spectral-flux onsets.
#include "dsp/contour.h"

#include "index.h"
#include <algorithm>
#include <array>
#include <cmath>

static const int HARMONICS = 8;
static const double HARMONIC_DECAY = 0.85; // weight of harmonic h is DECAY^(h-1)
static const int CANDIDATES = 5;           // pitch candidates kept per frame for the Viterbi pass
static const double MIN_HARMONICITY = 0.3; // normalised harmonic score needed to count as voiced
static const double LEVEL_RANGE_DB = 40;   // voiced frames are within this of the loud (p90) frames
static const double JUMP_COST = 0.3; // Viterbi cost per semitone of pitch change between frames

// Continuous MIDI pitch of a (fractional) bin index; the inverse of Spectrogram::binOfPitch.
static double pitchOfBin(const Spectrogram &s, double bin) {
    return 69 + 12 * log2(s.p.fmin / s.p.a4) + 12 * bin / s.p.binsPerOctave;
}

// Upper median (0 when empty).
static double median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

Contour trackPitch(const Spectrogram &s, double lowPitch, double highPitch) {
    Contour out;
    out.pitch.assign(idx(s.frames), -1);
    out.strength.assign(idx(s.frames), 0);
    const int bpo = s.p.binsPerOctave;
    // candidate fundamentals: bins k0..k1
    const int k0 = std::max(0, static_cast<int>(ceil(s.binOfPitch(lowPitch))));
    const int k1 = std::min(s.bins - 2, static_cast<int>(floor(s.binOfPitch(highPitch))));
    if (s.frames == 0 || k0 >= k1) {
        return out;
    }

    // harmonic h is off[h] bins above the fundamental, weighted w[h]
    std::array<double, HARMONICS + 1> off{}, w{};
    double wsum = 0;
    for (int h = 1; h <= HARMONICS; h++) {
        off[idx(h)] = bpo * log2(static_cast<double>(h));
        w[idx(h)] = pow(HARMONIC_DECAY, h - 1);
        wsum += w[idx(h)];
    }

    // Per frame: the best-scoring local maxima of the harmonic sum, and the level (dB).
    struct Cand {
        double pitch, score;
    };
    std::vector<std::vector<Cand>> cands(idx(s.frames));
    std::vector<double> level(idx(s.frames));
    std::vector<float> c(idx(s.bins)), score(idx(s.bins));
    for (int f = 0; f < s.frames; f++) {
        float peak = 0;
        for (int b = 0; b < s.bins; b++) {
            c[idx(b)] = sqrtf(s.at(f, b));
            if (b >= k0) {
                peak = std::max(peak, c[idx(b)]);
            }
        }
        // c = sqrt(mag), so 40 log10 = dB of mag
        level[idx(f)] = static_cast<double>(40 * log10(std::max(peak, 1e-6f)));
        if (peak <= 0) {
            continue;
        }

        // weighted sum of the (linearly interpolated) harmonics, relative to the frame's peak
        for (int k = k0; k <= k1; k++) {
            double sum = 0;
            for (int h = 1; h <= HARMONICS; h++) {
                const double b = k + off[idx(h)];
                const int i = static_cast<int>(b);
                if (i + 1 >= s.bins) {
                    break;
                }
                sum += w[idx(h)] * (static_cast<double>(c[idx(i)]) +
                                    static_cast<double>(c[idx(i + 1)] - c[idx(i)]) * (b - i));
            }
            score[idx(k)] = static_cast<float>(sum / (wsum * static_cast<double>(peak)));
        }

        std::vector<Cand> &cf = cands[idx(f)];
        for (int k = k0 + 1; k < k1; k++) {
            if (score[idx(k)] >= score[idx(k - 1)] && score[idx(k)] > score[idx(k + 1)]) {
                // parabolic interpolation for a fractional bin
                const double a = static_cast<double>(score[idx(k - 1)]);
                const double b = static_cast<double>(score[idx(k)]);
                const double d = static_cast<double>(score[idx(k + 1)]);
                const double den = a - 2 * b + d;
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

    // voiced: harmonic enough and not too far below the loud frames
    std::vector<double> lv = level;
    std::nth_element(lv.begin(), lv.begin() + lv.size() * 9 / 10, lv.end());
    const double minLevel = lv[lv.size() * 9 / 10] - LEVEL_RANGE_DB;
    std::vector<bool> voiced(idx(s.frames));
    for (int f = 0; f < s.frames; f++) {
        voiced[idx(f)] = !cands[idx(f)].empty() && cands[idx(f)][0].score >= MIN_HARMONICITY &&
                         level[idx(f)] >= minLevel;
    }

    // Viterbi over each voiced run a..b: cost[f - a][j] is the cheapest path ending in candidate j
    // of frame f, from[f - a][j] the previous frame's candidate on it.
    for (int a = 0; a < s.frames;) {
        if (!voiced[idx(a)]) {
            a++;
            continue;
        }
        int b = a;
        while (b + 1 < s.frames && voiced[idx(b + 1)]) {
            b++;
        }

        std::vector<std::vector<double>> cost(idx(b - a + 1));
        std::vector<std::vector<int>> from(idx(b - a + 1));
        for (int f = a; f <= b; f++) {
            const std::vector<Cand> &cf = cands[idx(f)];
            cost[idx(f - a)].assign(cf.size(), 0);
            from[idx(f - a)].assign(cf.size(), -1);
            for (size_t j = 0; j < cf.size(); j++) {
                const double emit = -log(cf[j].score + 1e-3);
                if (f == a) {
                    cost[0][j] = emit;
                    continue;
                }
                const std::vector<Cand> &cp = cands[idx(f - 1)];
                double best = 1e30;
                for (size_t i = 0; i < cp.size(); i++) {
                    const double v =
                        cost[idx(f - a - 1)][i] + JUMP_COST * fabs(cf[j].pitch - cp[i].pitch);
                    if (v < best) {
                        best = v;
                        from[idx(f - a)][j] = static_cast<int>(i);
                    }
                }
                cost[idx(f - a)][j] = best + emit;
            }
        }

        // trace the cheapest path back from the run's last frame
        int j = static_cast<int>(std::min_element(cost.back().begin(), cost.back().end()) -
                                 cost.back().begin());
        for (int f = b; f >= a; f--) {
            out.pitch[idx(f)] = static_cast<float>(cands[idx(f)][idx(j)].pitch);
            out.strength[idx(f)] = static_cast<float>(cands[idx(f)][idx(j)].score);
            j = from[idx(f - a)][idx(j)];
        }
        a = b + 1;
    }
    return out;
}

bool contourMedian(const Contour &c, int f0, int f1, int skip, double *pitch) {
    f0 = std::max(0, f0);
    f1 = std::min(static_cast<int>(c.pitch.size()) - 1, f1);
    if (f1 < f0) {
        return false;
    }

    // voiced pitches: all of them, and those from `skip` frames in
    std::vector<double> all, late;
    for (int f = f0; f <= f1; f++) {
        if (c.pitch[idx(f)] >= 0) {
            all.push_back(static_cast<double>(c.pitch[idx(f)]));
            if (f >= f0 + skip) {
                late.push_back(static_cast<double>(c.pitch[idx(f)]));
            }
        }
    }
    if (all.size() * 2 < idx(f1) - idx(f0) + 1) {
        return false; // f1 >= f0 here
    }
    *pitch = median(late.size() * 2 >= all.size() ? late : all);
    return true;
}

std::vector<Segment> segmentContour(const Contour &c, double frameRate,
                                    const std::vector<int> *onsets, double minDuration,
                                    double tolerance, double skipStart) {
    const int n = static_cast<int>(c.pitch.size());
    const int half = std::max(1, static_cast<int>(lround(0.075 * frameRate))); // ~150 ms window

    // vibrato-smoothed pitch: mean of voiced frames in the window, within the same voiced run
    std::vector<double> smooth(idx(n), -1);
    for (int f = 0; f < n; f++) {
        if (c.pitch[idx(f)] < 0) {
            continue;
        }
        double sum = 0;
        int cnt = 0;
        for (int g = f; g >= std::max(0, f - half) && c.pitch[idx(g)] >= 0; g--) {
            sum += static_cast<double>(c.pitch[idx(g)]);
            cnt++;
        }
        for (int g = f + 1; g <= std::min(n - 1, f + half) && c.pitch[idx(g)] >= 0; g++) {
            sum += static_cast<double>(c.pitch[idx(g)]);
            cnt++;
        }
        smooth[idx(f)] = sum / cnt;
    }

    // Cut the smoothed contour where it leaves `tolerance` of the running mean of the current
    // segment, or goes unvoiced; close() keeps the pieces long enough and voiced enough.
    const int minFrames = std::max(1, static_cast<int>(lround(minDuration * frameRate)));
    const int skip = static_cast<int>(lround(skipStart * frameRate));
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
    int start = -1; // first frame of the open segment, -1 if none
    double sum = 0; // of its smoothed pitches
    for (int f = 0; f <= n; f++) {
        const bool v = f < n && smooth[idx(f)] >= 0;
        if (start >= 0 && (!v || fabs(smooth[idx(f)] - sum / (f - start)) > tolerance)) {
            close(start, f - 1);
            start = -1;
        }
        if (v && start < 0) {
            start = f;
            sum = 0;
        }
        if (v) {
            sum += smooth[idx(f)];
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
    const int maxLegatoGap = static_cast<int>(lround(0.3 * frameRate));
    for (size_t i = 0; i + 1 < merged.size(); i++) {
        Segment &a = merged[i], &b = merged[i + 1];
        bool voicedGap = true;
        for (int f = a.f1; f <= b.f0 && voicedGap; f++) {
            voicedGap = c.pitch[idx(f)] >= 0;
        }
        if (!voicedGap || b.f0 - a.f1 > maxLegatoGap) {
            continue;
        }
        const double mid = 0.5 * (a.pitch + b.pitch);
        int cross = b.f0;
        for (int f = a.f1; f <= b.f0; f++) {
            if ((static_cast<double>(c.pitch[idx(f)]) - mid) * (b.pitch - a.pitch) >= 0) {
                cross = f;
                break;
            }
        }
        a.f1 = std::max(a.f0, cross - 1);
        b.f0 = cross;
    }

    // starts after silence: snap to the nearest onset within 60 ms, after the previous segment
    if (onsets) {
        const int range = static_cast<int>(lround(0.06 * frameRate));
        for (size_t i = 0; i < merged.size(); i++) {
            Segment &sg = merged[i];
            const int lo = i > 0 ? merged[i - 1].f1 + 1 : 0;
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
    // positive spectral flux of log-compressed magnitudes; frame 0 has none
    std::vector<double> flux(idx(s.frames), 0);
    std::vector<float> prev(idx(s.bins)), cur(idx(s.bins));
    for (int f = 0; f < s.frames; f++) {
        double sum = 0;
        for (int b = 0; b < s.bins; b++) {
            cur[idx(b)] = log1pf(100.0f * s.at(f, b));
            sum += static_cast<double>(std::max(0.0f, cur[idx(b)] - prev[idx(b)]));
        }
        if (f > 0) {
            flux[idx(f)] = sum;
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

    // onsets: flux above the threshold and the largest within `around` frames (ties go to the
    // first), at least 50 ms apart
    const int around = 3;
    const int minGap = std::max(1, static_cast<int>(lround(0.05 * s.frameRate())));
    std::vector<int> out;
    for (int f = 1; f < s.frames; f++) {
        if (flux[idx(f)] <= thr) {
            continue;
        }
        bool peak = true;
        for (int g = std::max(0, f - around); g <= std::min(s.frames - 1, f + around) && peak;
             g++) {
            peak = g == f || flux[idx(g)] < flux[idx(f)] || (flux[idx(g)] == flux[idx(f)] && g > f);
        }
        if (peak && (out.empty() || f - out.back() >= minGap)) {
            out.push_back(f);
        }
    }
    return out;
}
