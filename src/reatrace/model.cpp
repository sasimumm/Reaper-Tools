// REAPER-free window logic: the settings text format, quantise, follow-playback paging, the
// isolate filter's pitch schedule, adding intervals and the window layout.
#include "model.h"

#include "index.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

// One stored setting: its name in the "name=value;" text and a pointer to exactly one of the
// fields d, b, i or s of a Settings.
struct Setting {
    const char *name = nullptr;
    double *d = nullptr;
    bool *b = nullptr;
    int *i = nullptr;
    std::string *s = nullptr; // text without '=' or ';'
    double lo = 0, hi = 0;    // loaded values are clamped to this range
};

// The fields of `s`, as stored (pointing into `s`).
static std::vector<Setting> fields(Settings &s) {
    return {
        {.name = "gamma", .d = &s.np.gamma, .lo = 3, .hi = 40},
        {.name = "fmin", .d = &s.np.fmin, .lo = 20, .hi = 500},
        {.name = "fmax", .d = &s.np.fmax, .lo = 1000, .hi = 11000},
        {.name = "a4", .d = &s.np.a4, .lo = 415, .hi = 466},
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
        {.name = "chordtones", .b = &s.chordTones},
        {.name = "contour", .b = &s.showContour},
        {.name = "suggest", .b = &s.suggest},
        {.name = "suggestfrom", .i = &s.suggestFrom, .lo = 0, .hi = FROM_REFERENCE},
        {.name = "reference", .s = &s.reference},
        {.name = "referenceopacity", .d = &s.referenceOpacity, .lo = 0, .hi = 1},
        {.name = "estimatevote", .b = &s.estimateVote},
        {.name = "audition", .b = &s.audition},
        {.name = "theme", .b = &s.followTheme},
        {.name = "linkarrange", .b = &s.linkArrange},
        {.name = "followplay", .b = &s.followPlay},
        {.name = "isolatewidth", .d = &s.isolateWidth, .lo = 20, .hi = 400},
        {.name = "isolatebg", .d = &s.isolateBackground, .lo = -60, .hi = 0},
        {.name = "isolateall", .b = &s.isolateAll},
        {.name = "isolateduck", .b = &s.isolateDuck},
        {.name = "snap", .i = &s.snapMode, .lo = 0, .hi = SNAP_GRID},
        {.name = "griddiv", .i = &s.gridDiv, .lo = 0, .hi = 64},
        {.name = "gridtriplets", .b = &s.gridTriplets},
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
        } else if (st.s) {
            value = *st.s;
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
        const std::string text(p.substr(eq + 1, end - eq - 1));
        const double v = toNumber(text);
        for (const Setting &st : fs) {
            if (name == st.name && st.s) {
                *st.s = text;
            } else if (name == st.name) {
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

bool quantiseNotes(std::span<Note> notes, const std::function<double(double)> &snap,
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

// A quality word or letter: M and m by case, the rest in any case.
static std::optional<IntervalQuality> qualityNamed(std::string_view w) {
    if (w == "M") {
        return IQ_MAJOR;
    }
    if (w == "m") {
        return IQ_MINOR;
    }
    std::string l(w);
    std::transform(l.begin(), l.end(), l.begin(), [](char c) {
        return static_cast<char>(tolower(static_cast<unsigned char>(c)));
    });
    static const std::pair<const char *, IntervalQuality> NAMES[] = {
        {"", IQ_DIATONIC},           {"diat", IQ_DIATONIC},
        {"diatonic", IQ_DIATONIC},   {"maj", IQ_MAJOR},
        {"major", IQ_MAJOR},         {"min", IQ_MINOR},
        {"minor", IQ_MINOR},         {"p", IQ_PERFECT},
        {"per", IQ_PERFECT},         {"perf", IQ_PERFECT},
        {"perfect", IQ_PERFECT},     {"d", IQ_DIMINISHED},
        {"dim", IQ_DIMINISHED},      {"diminished", IQ_DIMINISHED},
        {"a", IQ_AUGMENTED},         {"aug", IQ_AUGMENTED},
        {"augmented", IQ_AUGMENTED},
    };
    for (const auto &[name, q] : NAMES) {
        if (l == name) {
            return q;
        }
    }
    return std::nullopt;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

// unisons, fourths, fifths and octaves are perfect; the others major or minor
static bool perfectDegree(int n) {
    const int simple = (n - 1) % 7;
    return simple == 0 || simple == 3 || simple == 4;
}

// A note name: "A", "Bb", "f#" as an Interval with its pitch class; with an octave, "A3" (57),
// "F#2", its pitch too. The letter must be uppercase then: a3 and d5 are intervals.
static std::optional<Interval> noteNamed(std::string_view e) {
    static const std::array PCS = {9, 11, 0, 2, 4, 5, 7}; // A..G
    const char letter = e.empty() ? 0 : e[0];
    const char c = static_cast<char>(tolower(letter));
    if (c < 'a' || c > 'g') {
        return std::nullopt;
    }
    e.remove_prefix(1);
    int pc = PCS[idx(c - 'a')];
    if (!e.empty() && (e[0] == 'b' || e[0] == '#')) {
        pc += e[0] == '#' ? 1 : -1;
        e.remove_prefix(1);
    }
    pc = (pc + 12) % 12;
    if (e.empty()) {
        return Interval{.degree = 1, .quality = IQ_DIATONIC, .pc = pc};
    }
    if (e.size() != 1 || e[0] < '0' || e[0] > '9' || letter != toupper(letter)) {
        return std::nullopt;
    }
    // the C of octave n is 12 * (n + 1); B# and Cb cross into the next and previous octaves
    const int pitch = pc + 12 * (e[0] - '0' + 1) + (c == 'b' && pc == 0 ? 12 : 0) -
                      (c == 'c' && pc == 11 ? 12 : 0);
    if (pitch > 127) {
        return std::nullopt;
    }
    return Interval{.degree = 1, .quality = IQ_DIATONIC, .pc = pc, .pitch = pitch};
}

std::optional<IntervalInput> parseIntervals(std::string_view text) {
    IntervalInput in;
    text = trim(text);
    if (!text.empty() && (text.front() == 't' || text.front() == 'T')) {
        in.transpose = true;
        text.remove_prefix(1);
    }
    while (true) {
        const size_t comma = text.find(',');
        std::string_view e = trim(text.substr(0, comma));
        int sign = 1;
        const bool hasSign = !e.empty() && (e.front() == '-' || e.front() == '+');
        if (hasSign) {
            sign = e.front() == '-' ? -1 : 1;
            e.remove_prefix(1);
        }
        if (std::optional<Interval> note = noteNamed(e)) {
            // unsigned when transposing: the nearest one
            note->degree = in.transpose && !hasSign ? 0 : sign;
            in.intervals.push_back(*note);
        } else {
            const size_t digits = e.find_first_of("0123456789");
            if (digits == e.npos || e.substr(digits).find_first_not_of("0123456789") != e.npos ||
                e.size() - digits > 2) {
                return std::nullopt;
            }
            const std::optional<IntervalQuality> q = qualityNamed(trim(e.substr(0, digits)));
            int n = 0; // one or two digits, checked above
            for (const char c : e.substr(digits)) {
                n = n * 10 + (c - '0');
            }
            if (!q || n < 1 || n > 22) { // up to three octaves
                return std::nullopt;
            }
            const bool perfect = perfectDegree(n);
            if ((perfect && (*q == IQ_MAJOR || *q == IQ_MINOR)) || (!perfect && *q == IQ_PERFECT)) {
                return std::nullopt; // no major fifth, no perfect third
            }
            in.intervals.push_back({sign * n, *q});
        }
        if (comma == text.npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return in;
}

int intervalPitch(int pitch, Interval iv, int root, int scale) {
    const int n = std::abs(iv.degree), dir = iv.degree < 0 ? -1 : 1;
    int to = 0;
    if (iv.pitch >= 0) {
        to = iv.pitch;
    } else if (iv.pc >= 0) {
        const int up = ((iv.pc - pitch) % 12 + 12) % 12, down = (12 - up) % 12;
        if (iv.degree > 0) {
            to = pitch + (up ? up : 12);
        } else if (iv.degree < 0) {
            to = pitch - (down ? down : 12);
        } else {
            to = up <= 6 ? pitch + up : pitch - down;
        }
    } else if (iv.quality == IQ_DIATONIC) {
        std::vector<int> pcs; // the scale's pitch classes above the root
        for (int b = 0; b < 12; b++) {
            if ((scale ? scale : 0xAB5) & (1 << b)) {
                pcs.push_back(b);
            }
        }
        const int m = static_cast<int>(pcs.size());
        const int rel = ((pitch - root) % 12 + 12) % 12;
        int base = pitch - rel; // the root at or below pitch
        int k = m - 1;          // the scale note at or below pitch
        while (k > 0 && pcs[idx(k)] > rel) {
            k--;
        }
        if (pcs[idx(k)] > rel) { // below the scale's lowest note: its last one an octave down
            k = m - 1;
            base -= 12;
        }
        const int offset = pitch - (base + pcs[idx(k)]);
        const int t = k + dir * (n - 1);
        const int oct = t >= 0 ? t / m : -((m - 1 - t) / m);
        to = base + oct * 12 + pcs[idx(t - oct * m)] + offset;
    } else {
        static const std::array MAJOR_OR_PERFECT = {0, 2, 4, 5, 7, 9, 11};
        int st = MAJOR_OR_PERFECT[idx((n - 1) % 7)] + 12 * ((n - 1) / 7);
        if (iv.quality == IQ_MINOR || (iv.quality == IQ_DIMINISHED && perfectDegree(n))) {
            st -= 1;
        } else if (iv.quality == IQ_DIMINISHED) {
            st -= 2;
        } else if (iv.quality == IQ_AUGMENTED) {
            st += 1;
        }
        to = pitch + dir * st;
    }
    return to >= 0 && to <= 127 ? to : -1;
}

bool addIntervals(std::vector<Note> &notes, const IntervalInput &in, int root, int scale) {
    if (in.intervals.empty()) {
        return false;
    }
    if (in.transpose) {
        std::vector<Note> out = notes;
        bool any = false;
        for (Note &n : out) {
            if (n.sel) {
                n.pitch = intervalPitch(n.pitch, in.intervals[0], root, scale);
                if (n.pitch < 0) {
                    return false;
                }
                any = true;
            }
        }
        if (any) {
            notes = std::move(out);
        }
        return any;
    }

    const auto sameStart = [](const Note &a, const Note &b) {
        return fabs(a.start - b.start) < 1e-6;
    };
    // the selected note at i is the highest (dir 1) or lowest (-1) selected one at its start
    const auto outer = [&](size_t i, int dir) {
        for (size_t j = 0; j < notes.size(); j++) {
            const Note &o = notes[j];
            if (j != i && o.sel && sameStart(o, notes[i]) &&
                (dir * (o.pitch - notes[i].pitch) > 0 || (o.pitch == notes[i].pitch && j < i))) {
                return false;
            }
        }
        return true;
    };
    const size_t count = notes.size();
    std::vector<std::pair<size_t, int>> bases; // (note, direction), chosen before adding any
    for (size_t i = 0; i < notes.size(); i++) {
        for (const int dir : {1, -1}) {
            if (notes[i].sel && outer(i, dir)) {
                bases.emplace_back(i, dir);
            }
        }
    }
    bool any = false;
    for (const auto &[i, dir] : bases) {
        for (const Interval &iv : in.intervals) {
            if ((iv.degree < 0 ? -1 : 1) != dir) {
                continue;
            }
            const int p = intervalPitch(notes[i].pitch, iv, root, scale);
            const bool taken = std::any_of(notes.begin(), notes.end(), [&](const Note &o) {
                return o.pitch == p && sameStart(o, notes[i]);
            });
            if (p < 0 || taken) {
                continue;
            }
            Note c = notes[i];
            c.pitch = p;
            c.idx = c.lyricIdx = -1;
            c.lyric.clear();
            c.sel = true;
            notes.push_back(c);
            any = true;
        }
    }
    // the added notes become the selection, so the next intervals go from them
    for (size_t i = 0; any && i < count; i++) {
        notes[i].sel = false;
    }
    return any;
}

// A note letter with an optional sharp or flat (#, b, or the Unicode signs) as a pitch class,
// consumed from s; -1 if s doesn't start with one.
static int notePitchClass(std::string_view &s) {
    static const std::array LETTERS = {9, 11, 0, 2, 4, 5, 7}; // A..G
    if (s.empty() || s.front() < 'A' || s.front() > 'G') {
        return -1;
    }
    int pc = LETTERS[idx(s.front() - 'A')];
    s.remove_prefix(1);
    if (s.starts_with('#') || s.starts_with('b')) {
        pc += s.front() == '#' ? 1 : -1;
        s.remove_prefix(1);
    } else if (s.starts_with("\u266F") || s.starts_with("\u266D")) { // sharp, flat sign
        pc += s.starts_with("\u266F") ? 1 : -1;
        s.remove_prefix(std::string_view("\u266F").size());
    }
    return (pc + 12) % 12;
}

std::optional<ChordTones> chordTones(std::string_view name) {
    ChordTones c;
    name = trim(name);
    c.root = notePitchClass(name);
    if (c.root < 0) {
        return std::nullopt;
    }
    // a slash bass: the last '/' followed by a note
    if (const size_t slash = name.rfind('/'); slash != name.npos) {
        std::string_view b = name.substr(slash + 1);
        if (const int bass = notePitchClass(b); bass >= 0 && b.empty()) {
            c.bass = bass;
            name = name.substr(0, slash);
        }
    }

    int third = 4, fifth = 7, seventh = -1; // semitones above the root; -1 for none
    unsigned extra = 0;
    bool dim = false;
    const auto add = [&](std::initializer_list<int> st) {
        for (const int s : st) {
            extra |= 1u << s;
        }
    };
    const auto ext = [&](int sev) { seventh = seventh >= 0 ? seventh : sev; };
    // longer tokens first where one starts another
    while (!name.empty()) {
        const auto take = [&](std::string_view t) {
            if (!name.starts_with(t)) {
                return false;
            }
            name.remove_prefix(t.size());
            return true;
        };
        if (take("maj13") || take("M13")) {
            seventh = 11;
            add({2, 9});
        } else if (take("maj11") || take("M11")) {
            seventh = 11;
            add({2, 5});
        } else if (take("maj9") || take("M9")) {
            seventh = 11;
            add({2});
        } else if (take("maj7") || take("M7") || take("\u0394")) { // triangle
            take("7");
            seventh = 11;
        } else if (take("maj")) {                                   // just major: Cmaj, Cmaj6
        } else if (take("dim7") || take("\u00B07") || take("o7")) { // degree sign
            third = 3;
            fifth = 6;
            seventh = 9;
            dim = true;
        } else if (take("dim") || take("\u00B0") || take("o")) {
            third = 3;
            fifth = 6;
            dim = true;
        } else if (take("\u00F8")) { // half-diminished
            take("7");
            third = 3;
            fifth = 6;
            seventh = 10;
        } else if (take("min") || take("m") || take("-")) {
            third = 3;
        } else if (take("aug") || take("+")) {
            fifth = 8;
        } else if (take("sus2")) {
            third = -1;
            add({2});
        } else if (take("sus4") || take("sus")) {
            third = -1;
            add({5});
        } else if (take("add9") || take("add2")) {
            add({2});
        } else if (take("add11") || take("add4")) {
            add({5});
        } else if (take("add13") || take("add6")) {
            add({9});
        } else if (take("6/9") || take("69")) {
            add({9, 2});
        } else if (take("b5")) {
            fifth = 6;
        } else if (take("#5")) {
            fifth = 8;
        } else if (take("b9")) {
            add({1});
        } else if (take("#9")) {
            add({3});
        } else if (take("#11")) {
            add({6});
        } else if (take("b13")) {
            add({8});
        } else if (take("13")) {
            ext(10);
            add({2, 9});
        } else if (take("11")) {
            ext(10);
            add({2, 5});
        } else if (take("9")) {
            ext(10);
            add({2});
        } else if (take("7")) {
            ext(dim ? 9 : 10);
        } else if (take("6")) {
            add({9});
        } else if (take("5")) {
            third = -1; // a power chord
        } else if (!take("(") && !take(")") && !take(",") && !take(" ")) {
            return std::nullopt;
        }
    }
    c.tones = 1u | extra;
    for (const int s : {third, fifth, seventh}) {
        if (s >= 0) {
            c.tones |= 1u << s;
        }
    }
    return c;
}

std::optional<std::pair<int, int>> keySigLine(std::string_view line) {
    line = trim(line); // also the \r of a project saved with CRLF line ends
    std::array<std::string_view, 4> f;
    for (std::string_view &field : f) {
        line = trim(line);
        const size_t end = std::min(line.size(), line.find_first_of(" \t"));
        field = line.substr(0, end);
        line.remove_prefix(end);
    }
    int root = -1, scale = 0;
    std::string_view hex = f[3];
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        hex.remove_prefix(2);
    }
    // a whole field of digits in base 10 or 16, at most 0xffff
    const auto num = [](std::string_view s, int *out, int base) {
        *out = 0;
        for (const char ch : s) {
            const int c = tolower(static_cast<unsigned char>(ch));
            const int d = isdigit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : base;
            if (d >= base || *out > 0xffff) {
                return false;
            }
            *out = *out * base + d;
        }
        return !s.empty();
    };
    if (!trim(line).empty() || !num(f[1], &root, 10) || !num(hex, &scale, 16) || root < 0 ||
        root > 11 || scale <= 0 || scale > 0xfff) {
        return std::nullopt;
    }
    return std::make_pair(root, scale);
}
