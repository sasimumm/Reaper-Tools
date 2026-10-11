#include "paint.h"

#include "analysis.h"
#include "chords.h"
#include "commands.h"
#include "draw.h"
#include "lyrics.h"
#include "panel.h"
#include "toolbar.h"

static const int GUIDE_THICKNESS = 3; // overtone hover marks, pixels

// Colours, 0xRRGGBB.
// The note area stays dark under the spectrogram; the rest follows REAPER's theme (applyTheme).
static const unsigned int C_AREA = 0x1b1b1b;
// Voices in the Okabe-Ito palette (colour-blind safe), readable over the graphite spectrogram.
static const unsigned int VOICE_COLOURS[WINDOW_VOICES] = {0xe69f00, 0x56b4e9, 0xcc79a7, 0x009e73};
static const unsigned int C_CONTOUR = 0xf0e442; // yellow: never white like the play cursor
static const unsigned int C_OUTLINE = 0xf4f6ff; // selected notes
static const unsigned int C_CROWDED = 0x8b91a3; // suggestions with no voice left
static const double EXPLAIN_DIM = 0.25;
// Drawing happens in device pixels: g_scale per point (2 on Retina). Layout stays in points.
static HDC g_memdc;
static int g_memw, g_memh, g_scale = 1, g_fontScale;
static bool g_layerValid;
static HFONT g_font, g_mono;
static std::vector<unsigned int> g_layer; // rendered spectrogram for the note area
struct LayerKey {
    double t0, pps, pitchTop, rowH;
    int w, h, version, explain; // explain: 0 off, else notes version + harmonics
    bool operator==(const LayerKey &o) const {
        return t0 == o.t0 && pps == o.pps && pitchTop == o.pitchTop && rowH == o.rowH && w == o.w &&
               h == o.h && version == o.version && explain == o.explain;
    }
};
static LayerKey g_layerKey;

static unsigned int mix(unsigned int a, unsigned int b, double f) {
    unsigned int out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        out |= (unsigned int)lround(((a >> sh) & 255) * (1 - f) + ((b >> sh) & 255) * f) << sh;
    }
    return out;
}

// Theme colours by their reaper.ini keys, falling back to the built-in dark colours.
static void applyTheme() {
    auto col = [](const char *key, unsigned int dflt) -> unsigned int {
        const int c = g_settings.followTheme ? GetThemeColor(key, 0) : -1;
        return c < 0 ? dflt
                     : ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255); // COLORREF is 0xBBGGRR
    };
    g_theme.bg = col("col_main_bg2", 0x1b1b1b), g_theme.panel = col("col_main_bg", 0x222222);
    g_theme.ruler = col("col_tl_bg", 0x262626), g_theme.text = col("col_main_text2", 0xd0d0d0);
    g_theme.dim = mix(g_theme.text, g_theme.panel, 0.4),
    g_theme.button = mix(g_theme.panel, g_theme.text, 0.1),
    g_theme.track = mix(g_theme.panel, g_theme.text, 0.2);
}

// Renders the note area at device resolution: g_scale pixels per point.
static void renderLayer(const Rect &n) {
    const double S = g_scale;
    const int w = (n.r - n.l) * g_scale, h = (n.b - n.t) * g_scale;
    g_layer.assign((size_t)w * h, 0xff000000 | C_AREA);
    const Spectrogram &s = g_analysis.spec;
    if (!s.frames || g_analysis.val.size() != s.mag.size()) {
        return;
    }

    std::vector<int> rowLo(h), rowHi(h); // bins covered by each pixel row
    for (int y = 0; y < h; y++) {
        const double hi = s.binOfPitch(g_ui.view.p(n.t + y / S)),
                     lo = s.binOfPitch(g_ui.view.p(n.t + (y + 1) / S));
        int a = (int)ceil(lo), b = (int)ceil(hi) - 1;
        if (a > b) {
            a = b = (int)lround(0.5 * (lo + hi));
        }
        rowLo[y] = std::max(0, a), rowHi[y] = std::min(s.bins - 1, b);
    }
    std::vector<double> rowPitch(h);
    for (int y = 0; y < h; y++) {
        rowPitch[y] = g_ui.view.p(n.t + (y + 0.5) / S);
    }
    std::vector<double> explained; // pitches of notes and overtones sounding in this column
    const int nh = (int)lround(g_settings.harmonics);
    std::vector<unsigned char> col(s.bins);
    for (int x = 0; x < w; x++) {
        explained.clear();
        if (g_settings.explain) {
            const double ta = g_ui.view.t(n.l + x / S), tb = g_ui.view.t(n.l + (x + 1) / S);
            auto add = [&](const Note &nt) {
                if (nt.start < tb && nt.end > ta) {
                    for (int hh = 1; hh <= nh; hh++) {
                        explained.push_back(nt.pitch + 12 * log2((double)hh));
                    }
                }
            };
            for (const Note &nt : g_session.ed.notes) {
                add(nt);
            }
            for (int v = 0; v < WINDOW_VOICES; v++) {
                if (v != g_session.voice) {
                    for (const Note &nt : g_session.others[v].notes) {
                        add(nt);
                    }
                }
            }
        }
        const double fa = (g_ui.view.t(n.l + x / S) - g_session.audioPos) * s.frameRate();
        const double fb = (g_ui.view.t(n.l + (x + 1) / S) - g_session.audioPos) * s.frameRate();
        int f0 = (int)ceil(fa), f1 = (int)ceil(fb) - 1;
        if (f0 > f1) {
            f0 = f1 = (int)lround(0.5 * (fa + fb));
        }
        f0 = std::max(f0, 0), f1 = std::min(f1, s.frames - 1);
        if (f0 > f1) {
            continue;
        }
        memcpy(col.data(), &g_analysis.val[(size_t)f0 * s.bins], s.bins);
        for (int f = f0 + 1; f <= f1; f++) {
            const unsigned char *v = &g_analysis.val[(size_t)f * s.bins];
            for (int b = 0; b < s.bins; b++) {
                col[b] = std::max(col[b], v[b]);
            }
        }
        for (int y = 0; y < h; y++) {
            if (rowLo[y] > rowHi[y]) {
                continue;
            }
            unsigned char m = 0;
            for (int k = rowLo[y]; k <= rowHi[y]; k++) {
                m = std::max(m, col[k]);
            }
            for (double ep : explained) {
                if (fabs(rowPitch[y] - ep) < 0.5) {
                    m = (unsigned char)(m * EXPLAIN_DIM);
                    break;
                }
            }
            g_layer[(size_t)y * w + x] = 0xff000000 | g_analysis.lut[m];
        }
    }
}

// The overview: the whole item over the overview's width, vocal range E2..C6 over its height.
static const int OV_LOW = 40, OV_HIGH = 84;
static std::vector<unsigned int> g_ovLayer;
static int g_ovW, g_ovH, g_ovVersion = -1;
static double g_ovPos, g_ovLen;

double overviewTime(int x) {
    const Rect o = overviewArea();
    return g_session.audioPos + g_session.audioLen * (x - o.l) / std::max(1, o.r - o.l);
}
static double overviewX(double t) {
    const Rect o = overviewArea();
    return o.l + (t - g_session.audioPos) / std::max(1e-9, g_session.audioLen) * (o.r - o.l);
}
static double overviewY(double pitch) {
    const Rect o = overviewArea();
    return o.b - 1 - (pitch - OV_LOW) / (OV_HIGH - OV_LOW) * (o.b - o.t - 2);
}

static void renderOverview(int w, int h) {
    g_ovLayer.assign((size_t)w * h, 0xff000000 | C_AREA);
    g_ovW = w, g_ovH = h, g_ovVersion = g_analysis.valVersion, g_ovPos = g_session.audioPos,
    g_ovLen = g_session.audioLen;
    const Spectrogram &s = g_analysis.spec;
    if (!s.frames || g_analysis.val.size() != s.mag.size()) {
        return;
    }
    const double lo = s.binOfPitch(OV_LOW - 0.5), hi = s.binOfPitch(OV_HIGH + 0.5);
    std::vector<unsigned char> col(s.bins);
    for (int x = 0; x < w; x++) {
        const int f0 = std::max(0, (int)((double)x * s.frames / w)),
                  f1 = std::min(s.frames, std::max(f0 + 1, (int)((double)(x + 1) * s.frames / w)));
        std::fill(col.begin(), col.end(), 0);
        for (int f = f0; f < f1; f++) {
            const unsigned char *v = &g_analysis.val[(size_t)f * s.bins];
            for (int b = 0; b < s.bins; b++) {
                col[b] = std::max(col[b], v[b]);
            }
        }
        for (int y = 0; y < h; y++) {
            const int b0 = std::max(0, (int)(hi - (hi - lo) * (y + 1) / h)),
                      b1 = std::min(s.bins - 1, (int)(hi - (hi - lo) * y / h));
            unsigned char m = 0;
            for (int b = b0; b <= b1; b++) {
                m = std::max(m, col[b]);
            }
            g_ovLayer[(size_t)y * w + x] = 0xff000000 | g_analysis.lut[m];
        }
    }
}

struct GridLine {
    int x, level;
    int measure;
}; // level: 0 = measure, 1 = beat, 2 = subdivision

static std::vector<GridLine> gridLines(const Rect &n) {
    std::vector<GridLine> out;
    const double qa = TimeMap2_timeToQN(NULL, g_ui.view.t(n.l)),
                 qb = TimeMap2_timeToQN(NULL, g_ui.view.t(n.r));
    const double step = gridStepQN();
    if (step > 0) {
        for (long long k = (long long)ceil(qa / step); (double)k * step <= qb && out.size() < 4000;
             k++) {
            const double q = (double)k * step;
            double ms = 0;
            const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &ms, NULL);
            const int level = fabs(q - ms) < 1e-6 ? 0 : fabs(q - floor(q + 0.5)) < 1e-6 ? 1 : 2;
            out.push_back({(int)lround(g_ui.view.x(TimeMap2_QNToTime(NULL, q))), level, m});
        }
    } else {
        double ms = 0, me = 0;
        TimeMap_QNToMeasures(NULL, qa, &ms, &me);
        for (double q = ms; q <= qb && out.size() < 4000;) {
            double s2 = 0, e2 = 0;
            const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &s2, &e2);
            out.push_back({(int)lround(g_ui.view.x(TimeMap2_QNToTime(NULL, s2))), 0, m});
            q = std::max(e2, q + 1e-3);
        }
    }
    return out;
}

void text(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col, int flags) {
    SetTextColor(dc, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
    RECT rc = {l * g_scale, t * g_scale, r * g_scale, b * g_scale};
    DrawText(dc, s, -1, &rc, flags | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
}

static void pitchName(int p, char *buf, int sz) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, sz, "%s%d", names[((p % 12) + 12) % 12], p / 12 - 1);
}
static bool isBlack(int p) { return (0x54A >> (((p % 12) + 12) % 12)) & 1; }

// --- overtone guides ------------------------------------------------------------------

// Strongest level (dB) within half a semitone of `pitch` between times a and b.
static double levelDb(double pitch, double a, double b) {
    const Spectrogram &s = g_analysis.spec;
    if (!s.frames) {
        return -200;
    }
    const int f0 = std::max(0, (int)floor((a - g_session.audioPos) * s.frameRate()));
    const int f1 = std::min(s.frames - 1, (int)ceil((b - g_session.audioPos) * s.frameRate()));
    const int b0 = std::max(0, (int)ceil(s.binOfPitch(pitch - 0.5))),
              b1 = std::min(s.bins - 1, (int)floor(s.binOfPitch(pitch + 0.5)));
    float m = 0;
    for (int f = f0; f <= f1; f++) {
        for (int k = b0; k <= b1; k++) {
            m = std::max(m, s.at(f, k));
        }
    }
    return 20 * log10(std::max(m, 1e-10f));
}

static double overtonePitch(double p, int h) { return p + 12 * log2((double)h); }

// Note ghosts: faint outlines where each note's overtones should be.
static void drawGhosts(Canvas &cv) {
    if (!g_settings.ghosts) {
        return;
    }
    const int a = (int)lround(g_settings.guideOpacity * 160), n = (int)lround(g_settings.harmonics);
    for (const Note &nt : g_session.ed.notes) {
        const int x0 = (int)lround(g_ui.view.x(nt.start)),
                  x1 = std::max(x0 + 2, (int)lround(g_ui.view.x(nt.end)));
        for (int h = 2; h <= n; h++) {
            const double y = g_ui.view.y(overtonePitch(nt.pitch, h)),
                         hh = std::max(1.0, g_ui.view.rowH * 0.3);
            const int y0 = (int)lround(y - hh), y1 = (int)lround(y + hh) + 1;
            cv.roundedFrame(x0, y0, x1, y1, VOICE_COLOURS[g_session.voice], 1, a / (h - 1) + a / 2);
        }
    }
}

// Hover guides: overtone marks for the pitch under the mouse (or the note being dragged), plus the
// octave check: the pitches this one would be the 2nd, 3rd or 4th overtone of, with their level
// relative to it. A strong one means the real note is probably lower.
static void drawHoverGuides(Canvas &cv, HDC dc, const Rect &n) {
    const int dn = g_session.ed.dragNote();
    if (dn < 0 && !n.in(g_ui.mouseX, g_ui.mouseY)) {
        return;
    }
    int p0 = 0;
    double tA = 0, tB = 0;
    int xA = 0, xB = 0;
    if (dn >= 0) {
        const Note &nt = g_session.ed.notes[dn];
        p0 = nt.pitch, tA = nt.start, tB = nt.end;
        xA = (int)lround(g_ui.view.x(tA)), xB = std::max(xA + 40, (int)lround(g_ui.view.x(tB)));
    } else {
        p0 = g_ui.view.pitchAt(g_ui.mouseY), tA = g_ui.view.t(g_ui.mouseX) - 0.03,
        tB = g_ui.view.t(g_ui.mouseX) + 0.03;
        xA = g_ui.mouseX - 70, xB = g_ui.mouseX + 70;
    }
    const int a = (int)lround(g_settings.guideOpacity * 256),
              nh = (int)lround(g_settings.harmonics);
    char buf[32];
    for (int h = 1; h <= nh; h++) {
        const int y = (int)lround(g_ui.view.y(overtonePitch(p0, h)));
        if (y < n.t || y >= n.b) {
            continue;
        }
        const int t = GUIDE_THICKNESS + (h == 1 ? 1 : 0);
        cv.fill(xA, y - t / 2, xB, y - t / 2 + t, VOICE_COLOURS[g_session.voice], h == 1 ? 256 : a);
        snprintf(buf, sizeof(buf), "%d", h);
        text(dc, buf, xB + 3, y - 8, xB + 30, y + 8, g_theme.text, DT_LEFT);
    }
    const double own = levelDb(p0, tA, tB);
    static const struct {
        int h;
        const char *label;
    } below[] = {{2, "8vb"}, {3, "12th"}, {4, "2 oct"}};
    for (const auto &c : below) {
        const double pc = overtonePitch(p0, 1) - 12 * log2((double)c.h);
        const int y = (int)lround(g_ui.view.y(pc));
        if (y < n.t || y >= n.b || g_analysis.spec.binOfPitch(pc) < 0) {
            continue;
        }
        const double rel = levelDb(pc, tA, tB) - own;
        const bool strong = rel > -12;
        for (int x = xA; x < xB; x += 8) {
            cv.fill(x, y - GUIDE_THICKNESS / 2, x + 4, y - GUIDE_THICKNESS / 2 + GUIDE_THICKNESS,
                    strong ? 0xffa040 : 0xa0a0a0, strong ? 256 : a);
        }
        snprintf(buf, sizeof(buf), "%s %+.0f dB", c.label, rel);
        text(dc, buf, xB + 3, y - 8, xB + 120, y + 8, strong ? 0xffa040 : 0xa0a0a0, DT_LEFT);
    }
}

static void drawSuggestions(Canvas &cv, const Rect &n) {
    if (!g_settings.suggest) {
        return;
    }
    const std::vector<Segment> &sug = suggestions();
    for (size_t i = 0; i < sug.size(); i++) {
        const Segment &sg = sug[i];
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        segmentRect(sg, &x0, &y0, &x1, &y1);
        if (x1 < n.l || x0 > n.r || covered(sg)) {
            continue;
        }
        const int v = suggestionVoice(i);
        if (v >= 0 && (g_ui.hiddenVoices >> v & 1)) {
            continue;
        }
        // a dashed outline in the colour of the voice A puts it in: not a note yet
        const unsigned int col = v >= 0 ? VOICE_COLOURS[v] : C_CROWDED;
        const int a = v >= 0 ? 220 : 160;
        for (int x = x0 + 1; x < x1 - 1; x += 7) {
            cv.fill(x, y0, std::min(x + 4, x1 - 1), y0 + 1, col, a),
                cv.fill(x, y1 - 1, std::min(x + 4, x1 - 1), y1, col, a);
        }
        for (int y = y0 + 1; y < y1 - 1; y += 5) {
            cv.fill(x0, y, x0 + 1, std::min(y + 3, y1 - 1), col, a),
                cv.fill(x1 - 1, y, x1, std::min(y + 3, y1 - 1), col, a);
        }
    }
}

// The sung pitch as a yellow line with a dark edge, one pixel column at a time.
static void drawContour(Canvas &cv, const Rect &n) {
    if (!g_settings.showContour || g_analysis.contour.pitch.empty()) {
        return;
    }
    const double fr = g_analysis.spec.frameRate();
    auto pitchAt = [&](double f) -> double { // linear between voiced frames, -1 if unvoiced
        const int a = (int)floor(f), b = a + 1;
        if (a < 0 || b >= (int)g_analysis.contour.pitch.size()) {
            return -1;
        }
        const double pa = g_analysis.contour.pitch[a], pb = g_analysis.contour.pitch[b];
        if (pa < 0 || pb < 0) {
            return pa >= 0 && f - a < 0.5 ? pa : pb >= 0 && f - a >= 0.5 ? pb : -1;
        }
        return pa + (pb - pa) * (f - a);
    };
    double prev = -1;
    for (int x = n.l; x < n.r; x++) {
        const double fa = (g_ui.view.t(x) - g_session.audioPos) * fr,
                     fb = (g_ui.view.t(x + 1) - g_session.audioPos) * fr;
        double lo = 1e9, hi = -1e9;
        auto add = [&](double p) {
            if (p >= 0) {
                lo = std::min(lo, p), hi = std::max(hi, p);
            }
        };
        add(pitchAt(fa)), add(pitchAt(fb));
        for (int f = (int)ceil(fa); f < fb; f++) {
            add(f >= 0 && f < (int)g_analysis.contour.pitch.size() ? g_analysis.contour.pitch[f]
                                                                   : -1);
        }
        if (hi < 0) {
            prev = -1;
            continue;
        }
        if (prev >= 0) {
            add(prev); // connect to the previous column
        }
        prev = pitchAt(fb) >= 0 ? pitchAt(fb) : hi;
        const int y0 = (int)lround(g_ui.view.y(hi)), y1 = (int)lround(g_ui.view.y(lo));
        cv.fill(x, y0 - 2, x + 1, y1 + 3, 0x000000, 150);
        cv.fill(x, y0 - 1, x + 1, y1 + 2, C_CONTOUR);
    }
}

// Notes with lyrics, by start time.
static std::vector<int> lyricOrder() {
    std::vector<int> out;
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        if (!g_session.ed.notes[i].lyric.empty()) {
            out.push_back((int)i);
        }
    }
    std::sort(out.begin(), out.end(), [](int a, int b) {
        return g_session.ed.notes[a].start < g_session.ed.notes[b].start;
    });
    return out;
}

// The note whose syllable is shown at x in the lyrics lane, -1 if none.
int lyricAt(int x) {
    const double t = g_ui.view.t(x);
    int best = -1;
    for (int i : lyricOrder()) {
        if (g_session.ed.notes[i].start <= t) {
            best = i;
        }
    }
    return best;
}

// --- status line, hints, empty states, shortcuts ---------------------------------------

static void monoText(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col,
                     int flags) {
    if (g_mono) {
        SelectObject(dc, g_mono);
    }
    text(dc, s, l, t, r, b, col, flags);
    if (g_font) {
        SelectObject(dc, g_font);
    }
}

// What a click or drag does where the mouse is.
static std::string hoverHint(const Layout &L) {
    const int x = g_ui.mouseX, y = g_ui.mouseY;
    static const char *const VOICE_NAMES[WINDOW_VOICES] = {"Lead", "voice 2", "voice 3", "voice 4"};
    const char *voice = VOICE_NAMES[g_session.voice];
    char buf[200];
    if (L.notes.in(x, y)) {
        int hit = -1;
        const NoteEditor::Hit h = g_session.ed.hitTest(g_ui.view, x, y, &hit);
        if (h == NoteEditor::LEFT || h == NoteEditor::RIGHT) {
            return "Drag: resize  ·  Alt: no snap";
        }
        if (h == NoteEditor::BODY) {
            return "Drag: move  ·  Shift: lock to time or pitch  ·  Double-click or Enter: lyric  "
                   "·  K: split";
        }
        if (const int i = suggestionAt(x, y); i >= 0) {
            const int v = suggestionVoice((size_t)i);
            if (v < 0) {
                snprintf(buf, sizeof(buf),
                         "Crowded out (more notes than voices)  ·  Click: add it to %s", voice);
            } else {
                snprintf(buf, sizeof(buf), "Click: accept into %s  ·  A: accept all in view",
                         VOICE_NAMES[v]);
            }
            return buf;
        }
        snprintf(buf, sizeof(buf),
                 "Drag: draw a note in %s  ·  Shift+drag: select  ·  Alt: no snap", voice);
        return buf;
    }
    if (const unsigned lane = laneLabelAt(x, y, L)) {
        return g_ui.foldedLanes & lane ? "Click: show this lane"
                                       : "Click: fold this lane to give the notes more room";
    }
    if (L.lyrics.in(x, y) && !(g_ui.foldedLanes & LANE_LYRICS)) {
        return "Click: edit this syllable  ·  Cmd+V: paste lyrics onto the selected notes";
    }
    if (L.overview.in(x, y) && !(g_ui.foldedLanes & LANE_OVERVIEW)) {
        return "Click or drag: scroll to that part of the item";
    }
    if (L.ruler.in(x, y)) {
        return "Click: move the edit cursor  ·  Shift+drag: time selection and loop  ·  R: loop it";
    }
    if (L.chords.in(x, y)) {
        return "Chords come from chord items: double-click one in the arrange view to edit it";
    }
    if (x >= 0 && x < KB_W && y >= L.notes.t && y < L.notes.b) {
        return "Click: hear this pitch";
    }
    if (L.toolbar.in(x, y)) {
        return toolbarTip(x, y, L);
    }
    return "?: shortcuts";
}

// A small rounded label on the status line; returns its left edge.
static int chip(Canvas &cv, HDC dc, int right, int t, int b, const char *label, unsigned int bg,
                unsigned int fg) {
    const int w = 12 + 7 * (int)strlen(label), l = right - w;
    cv.rounded(l, t + 3, right, b - 3, bg);
    text(dc, label, l, t, right, b, fg, DT_CENTER);
    return l;
}

// Fixed columns (pitch, Hz, peak cents, level, time, bar.beat, the note's sung offset), the hint,
// then the modes as chips on the right.
static void drawStatus(Canvas &cv, HDC dc, const Layout &L) {
    const Rect n = L.notes, s = L.status, c = L.client;
    cv.clip = {0, n.b, n.l, s.t};
    cv.fill(0, n.b, n.l, s.t, g_theme.button);
    cv.clip = s;
    cv.fill(s.l, s.t, s.r, s.b, g_theme.ruler);
    cv.fill(s.l, s.t, s.r, s.t + 1, 0x000000);
    char buf[160];
    static const int COLS[] = {44, 84, 112, 64, 92, 76, 100}; // widths, points
    std::string col[7];
    const bool over = n.in(g_ui.mouseX, g_ui.mouseY);
    if (over) {
        const double t = g_ui.view.t(g_ui.mouseX), p = g_ui.view.p(g_ui.mouseY);
        const int ip = (int)floor(p + 0.5);
        pitchName(ip, buf, sizeof(buf)), col[0] = buf;
        snprintf(buf, sizeof(buf), "%7.1f Hz", 440 * pow(2.0, (p - 69) / 12)), col[1] = buf;
        const Spectrogram &sp = g_analysis.spec;
        const int f = (int)lround((t - g_session.audioPos) * sp.frameRate());
        if (f >= 0 && f < sp.frames) {
            const int b0 = std::max(0, (int)floor(sp.binOfPitch(ip - 1))),
                      b1 = std::min(sp.bins - 1, (int)ceil(sp.binOfPitch(ip + 1)));
            int best = -1;
            for (int b = b0; b <= b1; b++) {
                if (best < 0 || sp.at(f, b) > sp.at(f, best)) {
                    best = b;
                }
            }
            if (best >= 0) {
                const double peak =
                    69 + 12 * log2(sp.p.fmin * pow(2.0, (double)best / sp.p.binsPerOctave) / 440);
                const int pp = (int)floor(peak + 0.5);
                char name[16];
                pitchName(pp, name, sizeof(name));
                snprintf(buf, sizeof(buf), "peak %-3s %+3.0fc", name, (peak - pp) * 100),
                    col[2] = buf;
                const int bin = std::max(0, std::min(sp.bins - 1, (int)lround(sp.binOfPitch(p))));
                snprintf(buf, sizeof(buf), "%4.0f dB", 20 * log10(std::max(1e-10f, sp.at(f, bin)))),
                    col[3] = buf;
            }
        }
        format_timestr_pos(t, buf, sizeof(buf), 0), col[4] = buf;
        format_timestr_pos(t, buf, sizeof(buf), 2), col[5] = buf;
        int hovered = -1;
        if (g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY, &hovered) !=
            NoteEditor::NONE) {
            const Note &hn = g_session.ed.notes[(size_t)hovered];
            double sung = 0;
            const int f0 = (int)lround((hn.start - g_session.audioPos) * sp.frameRate()),
                      f1 = (int)lround((hn.end - g_session.audioPos) * sp.frameRate());
            if (contourMedian(g_analysis.contour, f0, f1, (int)lround(0.05 * sp.frameRate()),
                              &sung)) {
                snprintf(buf, sizeof(buf), "sung %+.0fc", (sung - hn.pitch) * 100), col[6] = buf;
            }
        }
    }
    int x = 6;
    for (int i = 0; i < 7; i++) {
        monoText(dc, col[i].empty() ? (i < 2 && !over ? "—" : "") : col[i].c_str(), x, s.t,
                 x + COLS[i] - 6, s.b, i == 6 ? g_theme.dim : g_theme.text, DT_LEFT);
        x += COLS[i];
        cv.fill(x - 4, s.t + 5, x - 3, s.b - 5, g_theme.track);
    }

    // modes, right to left: progress or the analysis note, voice, snap
    static const char *const SNAP_NAMES[] = {"snap: off", "snap: onsets", "snap: grid"};
    static const char *const VOICE_NAMES[WINDOW_VOICES] = {"Lead", "Voice 2", "Voice 3", "Voice 4"};
    int right = c.r - 8;
    if (const int pm = analysisProgress(); pm >= 0) {
        cv.fill(right - 60, s.t + 10, right, s.b - 10, g_theme.button);
        cv.fill(right - 60, s.t + 10, right - 60 + 60 * pm / 1000, s.b - 10, C_ACCENT);
        right -= 66;
        snprintf(buf, sizeof(buf), "analysing %d%%", pm / 10);
        text(dc, buf, right - 90, s.t, right, s.b, g_theme.dim, DT_RIGHT);
        right -= 96;
    } else {
        const std::string &note =
            g_analysis.error.empty() ? g_analysis.estimateNote : g_analysis.error;
        if (!note.empty()) {
            text(dc, note.c_str(), x, s.t, right, s.b, 0xffa040, DT_RIGHT);
            right -= 8 + 6 * (int)note.size();
        }
    }
    right = chip(cv, dc, right, s.t, s.b, VOICE_NAMES[g_session.voice],
                 VOICE_COLOURS[g_session.voice], 0x14161c) -
            6;
    right = chip(cv, dc, right, s.t, s.b, SNAP_NAMES[std::clamp(g_settings.snapMode, 0, 2)],
                 g_theme.button, g_theme.dim) -
            10;
    if (right > x + 40) {
        text(dc, hoverHint(L).c_str(), x + 4, s.t, right, s.b, g_theme.dim,
             DT_LEFT | DT_END_ELLIPSIS);
    }
}

// What to do when there is nothing to see yet.
static void drawEmptyStates(Canvas &cv, HDC dc, const Rect &n) {
    cv.clip = n;
    const int cx = (n.l + n.r) / 2, cy = (n.t + n.b) / 2;
    auto banner = [&](const char *msg, int y) {
        const int w = 24 + 7 * (int)strlen(msg);
        cv.rounded(cx - w / 2, y - 14, cx + w / 2, y + 14, 0x000000, 150);
        text(dc, msg, cx - w / 2, y - 14, cx + w / 2, y + 14, g_theme.text, DT_CENTER);
    };
    char buf[300];
    if (!g_analysis.spec.frames) {
        if (const int pm = analysisProgress(); pm >= 0) {
            snprintf(buf, sizeof(buf), "Analysing…  %d%%", pm / 10);
            banner(buf, cy);
        } else if (!g_analysis.error.empty()) {
            banner(g_analysis.error.c_str(), cy);
        }
    } else if (g_session.ed.notes.empty() && !g_session.ed.active()) {
        banner("Drag to draw a note  ·  N: suggested notes  ·  ?: shortcuts", n.b - 30);
    }
}

// The shortcut sheet over the window (?, Esc to close).
static void drawShortcuts(Canvas &cv, HDC dc, const Layout &L) {
    struct Row {
        const char *what, *keys;
    };
    struct Group {
        const char *title;
        std::vector<Row> rows;
    };
    static const Group GROUPS[] = {
        {"Navigate",
         {{"Scroll pitch, time", "scroll, shift+scroll"},
          {"Zoom time, pitch", "cmd+scroll, alt+scroll"},
          {"Fit item, zoom to selection", "F, Z"},
          {"Link to arrange view", "L"}}},
        {"Draw and edit",
         {{"Draw a note", "drag on empty space"},
          {"Lock to time or pitch", "shift while moving"},
          {"No snapping", "alt"},
          {"Split, join", "K, J"},
          {"Copy, cut, paste, duplicate", "cmd+C, X, V, D"},
          {"Quantise selected", "Q"},
          {"Delete", "Delete, right-click"}}},
        {"Suggestions and voices",
         {{"Show suggestions", "N"},
          {"Accept all in view", "A"},
          {"Edit voice", "1-4"},
          {"Contour, ghosts, explain", "P, H, X"},
          {"Snap mode", "G"}}},
        {"Lyrics",
         {{"Type a syllable", "Enter, double-click"},
          {"Next, previous note", "Tab, Space, -; shift+Tab"},
          {"Paste lyrics", "cmd+V"}}},
        {"Listen",
         {{"Play, stop", "Space"}, {"Loop the selection", "R"}, {"Audio or notes only", "B"}}},
        {"Display",
         {{"Brightness, contrast", "[ ], - ="},
          {"Resolution", ", ."},
          {"Settings panel, dock", "S, D"}}},
    };
    const Rect c = L.client, n = L.notes;
    cv.clip = c;
    cv.fill(0, n.t, c.r, c.b, 0x0c0d11, 215);
    const int cols = 3, colW = std::min(300, (c.r - 80) / cols), x0 = (c.r - colW * cols) / 2;
    text(dc, "Shortcuts   (? or Esc to close)", x0, n.t + 14, x0 + colW * cols, n.t + 34,
         g_theme.text, DT_LEFT);
    for (int g = 0; g < 6; g++) {
        const int gx = x0 + (g % cols) * colW;
        int y = n.t + 46 + (g / cols) * 170;
        text(dc, GROUPS[g].title, gx, y, gx + colW, y + 18, C_ACCENT, DT_LEFT);
        y += 20;
        for (const Row &r : GROUPS[g].rows) {
            text(dc, r.what, gx, y, gx + colW - 10, y + 16, g_theme.text, DT_LEFT);
            monoText(dc, r.keys, gx, y, gx + colW - 14, y + 16, g_theme.dim, DT_RIGHT);
            y += 17;
        }
    }
}

// Each lane's label in the keyboard column; a folded lane is a thin strip.
static void drawLaneLabels(Canvas &cv, HDC dc, const Layout &L) {
    const struct {
        unsigned bit;
        Rect r;
        const char *label;
    } lanes[] = {
        {LANE_CHORDS, L.chords, "Chords"},
        {LANE_LYRICS, L.lyrics, "Lyrics"},
        {LANE_OVERVIEW, L.overview, "Overview"}
    };
    for (const auto &lane : lanes) {
        const Rect lr = {0, lane.r.t, KB_W, lane.r.b};
        cv.clip = {0, lane.r.t, L.client.r, lane.r.b};
        const bool hov = lr.in(g_ui.mouseX, g_ui.mouseY);
        cv.fill(lr.l, lr.t, lr.r, lr.b, hov ? g_theme.track : g_theme.button);
        if (g_ui.foldedLanes & lane.bit) {
            cv.fill(lane.r.l, lane.r.t, L.notes.r, lane.r.b, g_theme.ruler);
            cv.fill(8, (lr.t + lr.b) / 2 - 1, KB_W - 8, (lr.t + lr.b) / 2 + 1, g_theme.dim);
        } else {
            text(dc, lane.label, 6, lr.t, KB_W - 4, lr.b, hov ? g_theme.text : g_theme.dim,
                 DT_LEFT);
        }
        cv.fill(0, lr.t, L.notes.r, lr.t + 1, 0x000000);
    }
}

// The lane whose label is at (x, y), or 0.
unsigned laneLabelAt(int x, int y, const Layout &L) {
    if (x < 0 || x >= KB_W) {
        return 0;
    }
    if (y >= L.chords.t && y < L.chords.b) {
        return LANE_CHORDS;
    }
    if (y >= L.lyrics.t && y < L.lyrics.b) {
        return LANE_LYRICS;
    }
    if (y >= L.overview.t && y < L.overview.b) {
        return LANE_OVERVIEW;
    }
    return 0;
}

void paint(HDC hdc) {
    const Layout L = layout(); // once per frame, so every area agrees
    const Rect c = L.client;
    if (c.r < 2 || c.b < 2) {
        return;
    }
    applyTheme();
    g_scale = SWELL_IsRetinaHWND(g_ui.hwnd) ? 2 : 1;
    const int S = g_scale, w = (c.r * S + 3) & ~3,
              h = c.b * S; // SWELL backing stores are a multiple of 4 wide
    if (!g_memdc || w != g_memw || h != g_memh || g_fontScale != S) {
        if (g_memdc) {
            SWELL_DeleteGfxContext(g_memdc);
        }
        g_memdc = SWELL_CreateMemContext(hdc, w, h);
        g_memw = w, g_memh = h;
        if (g_font && g_fontScale != S) {
            DeleteObject(g_font), g_font = NULL;
        }
        if (g_mono && g_fontScale != S) {
            DeleteObject(g_mono), g_mono = NULL;
        }
        if (!g_font) {
            g_font = CreateFont(12 * S, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                DEFAULT_PITCH, "Helvetica");
        }
        if (!g_mono) { // figures that don't shift as the mouse moves
            g_mono = CreateFont(11 * S, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                DEFAULT_PITCH, "Menlo");
        }
        g_fontScale = S;
        if (g_memdc && g_font) {
            SelectObject(g_memdc, g_font);
        }
    }
    unsigned int *fb = g_memdc ? (unsigned int *)SWELL_GetCtxFrameBuffer(g_memdc) : NULL;
    if (!fb) {
        return;
    }
    if (L.tooSmall) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            fb[i] = 0xff000000 | g_theme.bg;
        }
        StretchBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, c.r * S, c.b * S, SRCCOPY);
        return;
    }
    syncViewOrigin();
    const Rect n = L.notes, ru = L.ruler;
    Canvas cv = {fb, w, h, {0, 0, c.r, c.b}, S};
    cv.fill(0, 0, c.r, c.b, g_theme.bg);
    HDC dc = g_memdc;
    SetBkMode(dc, TRANSPARENT);
    char buf[160];

    // spectrogram
    LayerKey key = {
        g_ui.view.t0,
        g_ui.view.pps,
        g_ui.view.pitchTop,
        g_ui.view.rowH,
        (n.r - n.l) * S,
        (n.b - n.t) * S,
        g_analysis.valVersion,
        g_settings.explain ? g_session.notesVersion * 32 + (int)lround(g_settings.harmonics) : 0
    };
    if (!g_layerValid || !(key == g_layerKey)) {
        renderLayer(n);
        g_layerKey = key, g_layerValid = true;
    }
    const int lw = (n.r - n.l) * S;
    for (int y = n.t * S; y < n.b * S; y++) {
        memcpy(fb + (size_t)y * w + (size_t)n.l * S, &g_layer[(size_t)(y - n.t * S) * lw],
               (size_t)lw * 4);
    }

    // pitch rows, then the time grid
    cv.clip = n;
    const int pLo = (int)floor(g_ui.view.p(n.b)), pHi = (int)ceil(g_ui.view.p(n.t));
    for (int p = pLo; p <= pHi; p++) {
        const int y = (int)lround(g_ui.view.y(p - 0.5)); // boundary below pitch p
        if (isBlack(p)) { // shaded like a piano roll, to read pitch away from the keyboard
            cv.fill(n.l, (int)lround(g_ui.view.y(p + 0.5)), n.r, y, 0x000000, 40);
        }
        cv.fill(n.l, y, n.r, y + 1, 0xffffff, p % 12 == 0 ? 50 : 10);
    }
    std::vector<GridLine> grid = gridLines(n);
    for (const GridLine &g : grid) {
        cv.fill(g.x, n.t, g.x + 1, n.b, 0xffffff, g.level == 0 ? 70 : g.level == 1 ? 32 : 14);
    }

    drawGhosts(cv);
    drawContour(cv, n);
    drawSuggestions(cv, n);
    drawHoverGuides(cv, dc, n);

    // other voices, dimmed and not editable
    if (g_ui.showOthers) {
        for (int v = 0; v < WINDOW_VOICES; v++) {
            if (g_ui.hiddenVoices >> v & 1) {
                continue;
            }
            for (const Note &nt : g_session.others[v].notes) {
                const int x0 = (int)lround(g_ui.view.x(nt.start)),
                          x1 = std::max(x0 + 2, (int)lround(g_ui.view.x(nt.end)));
                const int y0 = (int)lround(g_ui.view.y(nt.pitch + 0.5)),
                          y1 = std::max(y0 + 3, (int)lround(g_ui.view.y(nt.pitch - 0.5)));
                cv.rounded(x0, y0, x1, y1, VOICE_COLOURS[v], 70);
                cv.roundedFrame(x0, y0, x1, y1, VOICE_COLOURS[v]);
            }
        }
    }

    // notes
    const int drawing = g_session.ed.drawing();
    const unsigned int noteColour = VOICE_COLOURS[g_session.voice];
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        const Note &nt = g_session.ed.notes[i];
        const int x0 = (int)lround(g_ui.view.x(nt.start)),
                  x1 = std::max(x0 + 2, (int)lround(g_ui.view.x(nt.end)));
        const int y0 = (int)lround(g_ui.view.y(nt.pitch + 0.5)),
                  y1 = std::max(y0 + 3, (int)lround(g_ui.view.y(nt.pitch - 0.5)));
        // solid with a darker edge; selected ones get a light outline, so lyrics stay readable
        const bool sel = nt.sel || (int)i == drawing;
        cv.rounded(x0, y0, x1, y1, noteColour, 245);
        cv.roundedFrame(x0, y0, x1, y1, mix(noteColour, 0x000000, 0.55));
        if (sel) {
            cv.roundedFrame(x0 - 1, y0 - 1, x1 + 1, y1 + 1, C_OUTLINE, y1 - y0 > 6 ? 2 : 1);
        }
    }
    // lyrics: inside the note when it is tall enough, else just below it
    for (const Note &nt : g_session.ed.notes) {
        if (nt.lyric.empty()) {
            continue;
        }
        const int x0 = (int)lround(g_ui.view.x(nt.start)),
                  x1 = std::max(x0 + 2, (int)lround(g_ui.view.x(nt.end)));
        const int y0 = (int)lround(g_ui.view.y(nt.pitch + 0.5)),
                  y1 = (int)lround(g_ui.view.y(nt.pitch - 0.5));
        const int l = std::max(x0 + 2, n.l), r = std::min(std::max(x1, x0 + 60), n.r);
        if (r <= l) {
            continue;
        }
        if (y1 - y0 >= 12) {
            text(dc, nt.lyric.c_str(), l, std::max(y0, n.t), std::min(x1 - 1, n.r),
                 std::min(y1, n.b), mix(noteColour, 0x000000, 0.85), DT_LEFT);
        } else if (y1 + 14 <= n.b) {
            text(dc, nt.lyric.c_str(), l, y1, r, y1 + 14, 0xffffff, DT_LEFT);
        }
    }

    double mr[4];
    if (g_session.ed.marquee(mr)) {
        cv.fill((int)mr[0], (int)mr[1], (int)mr[2], (int)mr[3], 0xffffff, 30);
        cv.frame((int)mr[0], (int)mr[1], (int)mr[2], (int)mr[3], 0xffffff);
    }

    // keyboard
    cv.clip = {0, n.t, n.l, n.b};
    const int hoverPitch = n.in(g_ui.mouseX, g_ui.mouseY) ||
                                   (g_ui.mouseX < n.l && g_ui.mouseY >= n.t && g_ui.mouseY < n.b)
                               ? g_ui.view.pitchAt(g_ui.mouseY)
                               : -1;
    for (int p = pLo; p <= pHi; p++) {
        const int y0 = (int)lround(g_ui.view.y(p + 0.5)), y1 = (int)lround(g_ui.view.y(p - 0.5));
        cv.fill(0, y0, n.l, y1, p == hoverPitch ? C_ACCENT : isBlack(p) ? 0x303030 : 0xd8d8d8);
        cv.fill(0, y1 - 1, n.l, y1, 0x888888);
        if (p % 12 == 0 && y1 - y0 >= 8) {
            pitchName(p, buf, sizeof(buf));
            text(dc, buf, 2, y0, n.l - 4, y1, 0x202020, DT_RIGHT);
        }
    }
    cv.fill(n.l - 1, n.t, n.l, n.b, 0x000000);

    // ruler: bars on top, seconds below
    const int rt = ru.t; // under the toolbar
    cv.clip = {0, rt, ru.r, ru.b};
    cv.fill(0, rt, ru.r, ru.b, g_theme.ruler);
    cv.fill(0, ru.b - 1, ru.r, ru.b, 0x000000);
    int lastLabel = -1000;
    for (const GridLine &g : grid) {
        if (g.level > 1) {
            continue;
        }
        cv.fill(g.x, rt + (g.level == 0 ? 0 : 8), g.x + 1, rt + 14, 0xa0a0a0);
        if (g.level == 0 && g.x - lastLabel >= 36) {
            snprintf(buf, sizeof(buf), "%d", g.measure + 1);
            text(dc, buf, g.x + 3, rt, g.x + 60, rt + 14, g_theme.text, DT_LEFT);
            lastLabel = g.x;
        }
    }
    double secStep = 0.01;
    for (double s :
         {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0}) {
        if ((secStep = s) * g_ui.view.pps >= 70) {
            break;
        }
    }
    for (long long k = (long long)ceil(g_ui.view.t(n.l) / secStep);
         (double)k * secStep <= g_ui.view.t(n.r); k++) {
        const double t = (double)k * secStep;
        const int x = (int)lround(g_ui.view.x(t));
        cv.fill(x, rt + 22, x + 1, ru.b - 1, 0x707070);
        format_timestr_pos(t, buf, sizeof(buf), 0);
        monoText(dc, buf, x + 3, rt + 14, x + 90, ru.b - 1, g_theme.dim, DT_LEFT);
    }
    for (int f : g_analysis.onsets) {
        const int ox = (int)lround(g_ui.view.x(frameTime(f)));
        cv.fill(ox - 1, ru.b - 6, ox + 2, ru.b - 1, 0xffa040);
    }
    cv.clip = {0, rt, KB_W, ru.b};
    cv.fill(0, rt, KB_W, ru.b, g_theme.button);

    // chords lane: the chord items' chords, edited in the arrange view
    if (!(g_ui.foldedLanes & LANE_CHORDS)) {
        const Rect ch = L.chords;
        cv.clip = ch;
        cv.fill(ch.l, ch.t, ch.r, ch.b, g_theme.ruler);
        for (const ChordSpan &span : Chords_Spans()) {
            const int xa = (int)lround(g_ui.view.x(span.start)),
                      xb = (int)lround(g_ui.view.x(span.end));
            if (xb < ch.l || xa > ch.r) {
                continue;
            }
            cv.fill(xa, ch.t, xa + 1, ch.b, 0xa0a0a0);
            text(dc, span.name.c_str(), std::max(xa + 3, ch.l), ch.t, std::min(xb - 1, ch.r), ch.b,
                 g_theme.text, DT_LEFT);
        }
        cv.fill(ch.l, ch.b - 1, ch.r, ch.b, 0x000000);
    }

    // time selection
    cv.clip = {n.l, ru.t, n.r, n.b};
    double selA = 0, selB = 0;
    GetSet_LoopTimeRange2(NULL, false, false, &selA, &selB, false);
    if (selB > selA) {
        const int xa = (int)lround(g_ui.view.x(selA)), xb = (int)lround(g_ui.view.x(selB));
        cv.fill(xa, ru.t, xb, n.b, 0xffffff, 22);
        cv.fill(xa, ru.t, xa + 1, n.b, 0xffffff, 90), cv.fill(xb, ru.t, xb + 1, n.b, 0xffffff, 90);
    }

    // cursors
    int x = (int)lround(g_ui.view.x(GetCursorPosition()));
    cv.fill(x, ru.t, x + 1, n.b, C_EDIT_CURSOR);
    if (GetPlayState() & 1) {
        x = (int)lround(g_ui.view.x(GetPlayPosition2()));
        cv.fill(x, ru.t, x + 1, n.b, C_PLAY_CURSOR);
    }

    // lyrics lane: the visible lyric line as text, each syllable at its note's start
    if (!(g_ui.foldedLanes & LANE_LYRICS)) {
        const Rect ly = L.lyrics;
        cv.clip = ly;
        cv.fill(ly.l, ly.t, ly.r, ly.b, g_theme.ruler);
        const std::vector<int> order = lyricOrder();
        for (size_t k = 0; k < order.size(); k++) {
            const Note &nt = g_session.ed.notes[order[k]];
            const int sx = (int)lround(g_ui.view.x(nt.start));
            const int next = k + 1 < order.size()
                                 ? (int)lround(g_ui.view.x(g_session.ed.notes[order[k + 1]].start))
                                 : ly.r;
            if (next < ly.l || sx > ly.r) {
                continue;
            }
            cv.fill(sx, ly.t, sx + 1, ly.t + 4, noteColour);
            text(dc, nt.lyric.c_str(), std::max(sx + 2, ly.l), ly.t, std::min(next - 1, ly.r), ly.b,
                 nt.sel ? 0xffffff : g_theme.text, DT_LEFT);
        }
    }

    // overview: whole item, notes, and the visible range
    if (!(g_ui.foldedLanes & LANE_OVERVIEW)) {
        const Rect o = L.overview;
        const int ow = (o.r - o.l) * S, oh = (o.b - o.t) * S;
        if (ow != g_ovW || oh != g_ovH || g_ovVersion != g_analysis.valVersion ||
            g_ovPos != g_session.audioPos || g_ovLen != g_session.audioLen) {
            renderOverview(ow, oh);
        }
        for (int y = 0; y < oh; y++) {
            memcpy(fb + (size_t)(o.t * S + y) * w + (size_t)o.l * S, &g_ovLayer[(size_t)y * ow],
                   (size_t)ow * 4);
        }
        cv.clip = o;
        for (const Note &nt : g_session.ed.notes) {
            const int y = (int)lround(overviewY(nt.pitch));
            cv.fill((int)lround(overviewX(nt.start)), y - 1,
                    std::max((int)lround(overviewX(nt.start)) + 1, (int)lround(overviewX(nt.end))),
                    y + 1, noteColour);
        }
        const int va = (int)lround(overviewX(g_ui.view.t(n.l))),
                  vb = std::max(va + 2, (int)lround(overviewX(g_ui.view.t(n.r))));
        cv.fill(o.l, o.t, va, o.b, 0x000000, 120), cv.fill(vb, o.t, o.r, o.b, 0x000000, 120);
        cv.frame(va, o.t, vb, o.b, 0xffffff);
        cv.fill(o.l, o.t, o.r, o.t + 1, 0x000000);
    }

    drawEmptyStates(cv, dc, n);
    drawStatus(cv, dc, L);
    drawLaneLabels(cv, dc, L);
    if (g_ui.help) {
        drawShortcuts(cv, dc, L);
    }

    cv.clip = {0, 0, c.r, c.b};
    if (L.panelShown) {
        drawPanel(cv, dc);
    }
    drawToolbar(cv, dc, L);

    StretchBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, c.r * S, c.b * S,
               SRCCOPY); // 2:1 is SWELL's retina blit
    if (g_ui.edit) {
        placeEdit(); // follow scrolling and zooming while typing a lyric
    }
}

void releasePaintCache() {
    if (g_memdc) {
        SWELL_DeleteGfxContext(g_memdc);
    }
    g_memdc = NULL, g_memw = g_memh = 0, g_layerValid = false;
}
