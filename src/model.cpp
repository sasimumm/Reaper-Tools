#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Setting {
    const char *name = nullptr;
    double *d = nullptr;
    bool *b = nullptr;
    int *i = nullptr;
    double lo = 0, hi = 0; // loaded values are clamped to this range
};
// The fields of `s`, as stored.
static std::vector<Setting> fields(Settings &s) {
    return {
        {.name = "gamma", .d = &s.np.gamma, .lo = 3, .hi = 40},
        {.name = "fmin", .d = &s.np.fmin, .lo = 20, .hi = 500},
        {.name = "fmax", .d = &s.np.fmax, .lo = 1000, .hi = 11000},
        {.name = "binspersemitone", .d = &s.binsPerSemitone, .lo = 1, .hi = 10},
        {.name = "channel", .i = &s.channel, .lo = 0, .hi = CH_CENTRE},
        {.name = "bass", .b = &s.np.bass},
        {.name = "basscrossover", .d = &s.np.bassCrossover, .lo = 80, .hi = 500},
        {.name = "floor", .d = &s.rp.dbFloor, .lo = -120, .hi = -20},
        {.name = "contrast", .d = &s.rp.contrast, .lo = 0.1, .hi = 3},
        {.name = "brightness", .d = &s.rp.brightness, .lo = -1, .hi = 1},
        {.name = "bias", .d = &s.rp.bias, .lo = -2, .hi = 2},
        {.name = "colormap", .i = &s.rp.colormap, .lo = 0, .hi = COLORMAP_MAGMA},
        {.name = "peaks", .b = &s.rp.peaksOnly},
        {.name = "peakdim", .d = &s.rp.peakDim, .lo = 0, .hi = 1},
        {.name = "denoise", .b = &s.rp.subtractNoise},
        {.name = "noisealpha", .d = &s.rp.noiseAlpha, .lo = 0, .hi = 3},
        {.name = "noisefrac", .d = &s.rp.noiseFraction, .lo = 0.01, .hi = 0.5},
        {.name = "ghosts", .b = &s.ghosts},
        {.name = "harmonics", .d = &s.harmonics, .lo = 2, .hi = 16},
        {.name = "guideopacity", .d = &s.guideOpacity, .lo = 0, .hi = 1},
        {.name = "explain", .b = &s.explain},
        {.name = "contour", .b = &s.showContour},
        {.name = "suggest", .b = &s.suggest},
        {.name = "suggestfrom", .i = &s.suggestFrom, .lo = 0, .hi = 1},
        {.name = "estimatevote", .b = &s.estimateVote},
        {.name = "pitchfromcontour", .b = &s.pitchFromContour},
        {.name = "audition", .b = &s.audition},
        {.name = "theme", .b = &s.followTheme},
        {.name = "linkarrange", .b = &s.linkArrange},
        {.name = "snap", .i = &s.snapMode, .lo = 0, .hi = SNAP_GRID},
        {.name = "quantise", .d = &s.quantStrength, .lo = 0, .hi = 1},
    };
}

std::string encodeSettings(const Settings &settings) {
    Settings copy = settings;
    std::string out;
    char buf[64];
    for (const Setting &st : fields(copy)) {
        if (st.d) {
            snprintf(buf, sizeof(buf), "%s=%.6g;", st.name, *st.d);
        } else if (st.b) {
            snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.b ? 1 : 0);
        } else if (st.i) {
            snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.i);
        } else {
            continue;
        }
        out += buf;
    }
    return out;
}

void decodeSettings(const char *str, Settings &settings) {
    const std::vector<Setting> fs = fields(settings);
    for (const char *p = str; p && *p;) {
        const char *eq = strchr(p, '='), *end = strchr(p, ';');
        if (!eq || !end || eq > end) {
            break;
        }
        const std::string name(p, eq - p);
        const double v = strtod(eq + 1, NULL);
        for (const Setting &st : fs) {
            if (name == st.name) {
                const double c = v >= st.lo ? std::min(v, st.hi) : st.lo; // NaN -> lo
                if (st.d) {
                    *st.d = c;
                } else if (st.b) {
                    *st.b = v != 0;
                } else if (st.i) {
                    *st.i = (int)c;
                }
            }
        }
        p = end + 1;
    }
}

bool quantiseNotes(std::vector<Note> &notes, const std::function<double(double)> &snap,
                   double strength, double minLen) {
    bool any = false;
    for (Note &n : notes) {
        if (!n.sel) {
            continue;
        }
        const double s = n.start + (snap(n.start) - n.start) * strength;
        double e = n.end + (snap(n.end) - n.end) * strength;
        if (e < s + minLen) {
            e = std::max(n.end - n.start + s, s + minLen);
        }
        any |= s != n.start || e != n.end;
        n.start = s, n.end = e;
    }
    return any;
}

bool coversSuggestion(const Note &n, double start, double end, double pitch) {
    return fabs(n.pitch - pitch) <= 1 &&
           std::min(end, n.end) - std::max(start, n.start) > 0.3 * (end - start);
}

Layout computeLayout(int width, int height, bool panelWanted, unsigned folded) {
    const int chordsH = folded & LANE_CHORDS ? FOLDED_H : CHORDS_H,
              lyricsH = folded & LANE_LYRICS ? FOLDED_H : LYRICS_H,
              overviewH = folded & LANE_OVERVIEW ? FOLDED_H : OVERVIEW_H;
    Layout L = {};
    L.client = {0, 0, std::max(0, width), std::max(0, height)};
    const Rect &c = L.client;
    L.tooSmall = c.r < MIN_W || c.b < MIN_H;
    L.panelShown = panelWanted && c.r >= KB_W + PANEL_W + 100;
    const int top = TOOLBAR_H + RULER_H + chordsH;
    const Rect n = {
        KB_W, top, std::max(KB_W + 1, c.r - (L.panelShown ? PANEL_W : 0)),
        std::max(top + 1, c.b - STATUS_H - overviewH - lyricsH)
    };
    L.toolbar = {0, 0, c.r, TOOLBAR_H};
    L.notes = n;
    L.ruler = {n.l, TOOLBAR_H, n.r, TOOLBAR_H + RULER_H};
    L.chords = {n.l, TOOLBAR_H + RULER_H, n.r, n.t};
    L.lyrics = {n.l, n.b, n.r, n.b + lyricsH};
    L.overview = {n.l, n.b + lyricsH, n.r, n.b + lyricsH + overviewH};
    L.status = {0, L.overview.b, c.r, c.b};
    L.panel = {n.r, TOOLBAR_H, c.r, L.status.t};
    return L;
}
