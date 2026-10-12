// REAPER-free window logic: the settings text format, quantise, follow-playback paging, the
// isolate filter's pitch schedule and the window layout.
#include "model.h"

#include "index.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

// One stored setting: its name in the "name=value;" text and a pointer to exactly one of the
// fields d, b or i of a Settings.
struct Setting {
    const char *name = nullptr;
    double *d = nullptr;
    bool *b = nullptr;
    int *i = nullptr;
    double lo = 0, hi = 0; // loaded values are clamped to this range
};

// The fields of `s`, as stored (pointing into `s`).
static std::vector<Setting> fields(Settings &s) {
    return {
        {.name = "gamma", .d = &s.np.gamma, .lo = 3, .hi = 40},
        {.name = "fmin", .d = &s.np.fmin, .lo = 20, .hi = 500},
        {.name = "fmax", .d = &s.np.fmax, .lo = 1000, .hi = 11000},
        {.name = "binspersemitone", .d = &s.binsPerSemitone, .lo = 1, .hi = 10},
        {.name = "channel", .i = &s.channel, .lo = 0, .hi = CH_CENTRE},
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
        {.name = "followplay", .b = &s.followPlay},
        {.name = "isolatewidth", .d = &s.isolateWidth, .lo = 20, .hi = 400},
        {.name = "isolatebg", .d = &s.isolateBackground, .lo = -60, .hi = 0},
        {.name = "isolateall", .b = &s.isolateAll},
        {.name = "snap", .i = &s.snapMode, .lo = 0, .hi = SNAP_GRID},
        {.name = "quantise", .d = &s.quantStrength, .lo = 0, .hi = 1},
    };
}

// strtod on s, for the libc number syntax (0 if there is no number).
static double toNumber(const std::string &s) {
    // strtod only takes a C string.
#pragma clang unsafe_buffer_usage begin
    return strtod(s.c_str(), NULL);
#pragma clang unsafe_buffer_usage end
}

std::string encodeSettings(const Settings &settings) {
    Settings copy = settings; // fields() needs pointers to non-const fields
    std::string out;
    char buf[64];
    for (const Setting &st : fields(copy)) {
        std::string value;
        if (st.d) {
            snprintf(buf, sizeof(buf), "%.6g", *st.d);
            value = buf;
        } else if (st.b) {
            value = *st.b ? "1" : "0";
        } else if (st.i) {
            value = std::to_string(*st.i);
        } else {
            continue;
        }
        out += std::string(st.name) + "=" + value + ";";
    }
    return out;
}

void decodeSettings(const char *str, Settings &settings) {
    const std::vector<Setting> fs = fields(settings);
    // one "name=value;" pair per step; a malformed rest is ignored
    for (std::string_view p = str ? str : ""; !p.empty();) {
        const size_t eq = p.find('='), end = p.find(';');
        if (eq == p.npos || end == p.npos || eq > end) {
            break;
        }

        const std::string name(p.substr(0, eq));
        const double v = toNumber(std::string(p.substr(eq + 1, end - eq - 1)));
        for (const Setting &st : fs) {
            if (name == st.name) {
                const double c = v >= st.lo ? std::min(v, st.hi) : st.lo; // NaN -> lo
                if (st.d) {
                    *st.d = c;
                } else if (st.b) {
                    *st.b = v != 0;
                } else if (st.i) {
                    *st.i = static_cast<int>(c);
                }
            }
        }
        p.remove_prefix(end + 1);
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
        // too short (or reversed): keep the note's own length from the new start, at least minLen
        if (e < s + minLen) {
            e = std::max(n.end - n.start + s, s + minLen);
        }
        any |= s != n.start || e != n.end;
        n.start = s;
        n.end = e;
    }
    return any;
}

double followPage(double t0, double width, double prev, double now) {
    const auto in = [&](double t) { return t >= t0 && t < t0 + width; };
    return in(prev) && !in(now) ? now - width / 20 : t0;
}

std::vector<std::pair<double, int>> pitchSchedule(std::vector<Note> spans, double from,
                                                  size_t max) {
    std::sort(spans.begin(), spans.end(),
              [](const Note &a, const Note &b) { return a.start < b.start; });
    std::vector<std::pair<double, int>> out;
    for (const Note &n : spans) {
        if (n.end > from && out.size() < max) {
            out.emplace_back(n.start, n.pitch);
        }
    }
    return out;
}

bool coversSuggestion(const Note &n, double start, double end, double pitch) {
    return fabs(n.pitch - pitch) <= 1 &&
           std::min(end, n.end) - std::max(start, n.start) > 0.3 * (end - start);
}

Layout computeLayout(int width, int height, bool panelWanted, unsigned folded) {
    const int chordsH = folded & LANE_CHORDS ? FOLDED_H : CHORDS_H;
    const int lyricsH = folded & LANE_LYRICS ? FOLDED_H : LYRICS_H;
    const int overviewH = folded & LANE_OVERVIEW ? FOLDED_H : OVERVIEW_H;

    Layout L = {};
    L.client = {0, 0, std::max(0, width), std::max(0, height)};
    const Rect &c = L.client;
    L.tooSmall = c.r < MIN_W || c.b < MIN_H;
    L.panelShown = panelWanted && c.r >= KB_W + PANEL_W + 100; // at least 100 points of notes

    // the notes area, kept at least a point wide and high; the other areas line up with it
    const int top = TOOLBAR_H + RULER_H + chordsH;
    const int notesR = std::max(KB_W + 1, c.r - (L.panelShown ? PANEL_W : 0));
    const int notesB = std::max(top + 1, c.b - STATUS_H - overviewH - lyricsH);
    const Rect n = {KB_W, top, notesR, notesB};

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
