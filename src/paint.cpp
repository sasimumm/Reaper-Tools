#include "paint.h"

#include "analysis.h"
#include "chords.h"
#include "commands.h"
#include "draw.h"
#include "lyrics.h"
#include "panel.h"

static const int GUIDE_THICKNESS = 3; // overtone hover marks, pixels

// Colours, 0xRRGGBB.
// The note area stays dark under the spectrogram; the rest follows REAPER's theme (applyTheme).
static const unsigned int C_AREA = 0x1b1b1b;
static const unsigned int VOICE_COLOURS[WINDOW_VOICES] = {0x2ec4b6, 0xf4a261, 0xe56fd8, 0x9ee05a};
static const double EXPLAIN_DIM = 0.25;
// Drawing happens in device pixels: g_scale per point (2 on Retina). Layout stays in points.
static HDC g_memdc;
static int g_memw, g_memh, g_scale = 1, g_fontScale;
static bool g_layerValid;
static HFONT g_font;
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
// Below this size only the background is drawn, so the areas below always fit in the client rect.
static const int MIN_W = KB_W + 40,
                 MIN_H = RULER_H + CHORDS_H + LYRICS_H + OVERVIEW_H + STATUS_H + 20;

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
            const int y0 = (int)lround(y - hh), y1 = (int)lround(y + hh) + 2;
            const unsigned int col = VOICE_COLOURS[g_session.voice];
            cv.fill(x0, y0, x1, y0 + 2, col, a), cv.fill(x0, y1 - 2, x1, y1, col, a);
            cv.fill(x0, y0, x0 + 2, y1, col, a), cv.fill(x1 - 2, y0, x1, y1, col, a);
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
        cv.fill(xA, y - t / 2, xB, y - t / 2 + t, 0xffffff, h == 1 ? 256 : a);
        snprintf(buf, sizeof(buf), "%d", h);
        text(dc, buf, xB + 3, y - 8, xB + 30, y + 8, 0xffffff, DT_LEFT);
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
        const unsigned int col =
            v >= 0 ? VOICE_COLOURS[v] : 0x909090; // in the colour of the voice A puts it in
        cv.fill(x0, y0, x1, y1, col, 50);
        for (int x = x0; x < x1; x += 6) {
            cv.fill(x, y0, std::min(x + 3, x1), y0 + 1, col),
                cv.fill(x, y1 - 1, std::min(x + 3, x1), y1, col);
        }
        for (int y = y0; y < y1; y += 4) {
            cv.fill(x0, y, x0 + 1, std::min(y + 2, y1), col),
                cv.fill(x1 - 1, y, x1, std::min(y + 2, y1), col);
        }
    }
}

// The sung pitch as a white line with a dark edge, one pixel column at a time.
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
        cv.fill(x, y0 - 2, x + 1, y1 + 3, 0x000000, 180);
        cv.fill(x, y0 - 1, x + 1, y1 + 2, 0xffffff);
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

void paint(HDC hdc) {
    const Rect c = clientRect();
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
        if (!g_font) {
            g_font = CreateFont(12 * S, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                DEFAULT_PITCH, "Helvetica");
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
    if (c.r < MIN_W || c.b < MIN_H) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            fb[i] = 0xff000000 | g_theme.bg;
        }
        StretchBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, c.r * S, c.b * S, SRCCOPY);
        return;
    }
    syncViewOrigin();
    const Rect n = noteArea(), ru = rulerArea();
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
        cv.fill(n.l, y, n.r, y + 1, 0xffffff, p % 12 == 0 ? 60 : 16);
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
            for (const Note &nt : g_session.others[v].notes) {
                const int x0 = (int)lround(g_ui.view.x(nt.start)),
                          x1 = std::max(x0 + 2, (int)lround(g_ui.view.x(nt.end)));
                const int y0 = (int)lround(g_ui.view.y(nt.pitch + 0.5)),
                          y1 = std::max(y0 + 3, (int)lround(g_ui.view.y(nt.pitch - 0.5)));
                cv.fill(x0, y0, x1, y1, VOICE_COLOURS[v], 80);
                cv.frame(x0, y0, x1, y1, VOICE_COLOURS[v]);
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
        const bool sel = nt.sel || (int)i == drawing;
        cv.fill(x0, y0, x1, y1, sel ? C_SEL : noteColour, 235);
        cv.frame(x0, y0, x1, y1, sel ? noteColour : C_NOTE_EDGE, sel && y1 - y0 > 6 ? 2 : 1);
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
                 std::min(y1, n.b), nt.sel ? 0x0b3d3a : 0x04201e, DT_LEFT);
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
    cv.clip = {0, 0, ru.r, RULER_H};
    cv.fill(0, 0, ru.r, RULER_H, g_theme.ruler);
    cv.fill(0, RULER_H - 1, ru.r, RULER_H, 0x000000);
    int lastLabel = -1000;
    for (const GridLine &g : grid) {
        if (g.level > 1) {
            continue;
        }
        cv.fill(g.x, g.level == 0 ? 0 : 8, g.x + 1, 14, 0xa0a0a0);
        if (g.level == 0 && g.x - lastLabel >= 36) {
            snprintf(buf, sizeof(buf), "%d", g.measure + 1);
            text(dc, buf, g.x + 3, 0, g.x + 60, 14, g_theme.text, DT_LEFT);
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
        cv.fill(x, 22, x + 1, RULER_H - 1, 0x707070);
        format_timestr_pos(t, buf, sizeof(buf), 0);
        text(dc, buf, x + 3, 14, x + 90, RULER_H - 1, g_theme.dim, DT_LEFT);
    }
    for (int f : g_analysis.onsets) {
        const int ox = (int)lround(g_ui.view.x(frameTime(f)));
        cv.fill(ox - 1, RULER_H - 6, ox + 2, RULER_H - 1, 0xffa040);
    }
    cv.clip = {0, 0, KB_W, RULER_H};
    cv.fill(0, 0, KB_W, RULER_H, g_theme.button);
    text(dc, g_ui.panel ? "Hide (S)" : "Settings", 0, 0, KB_W, RULER_H, g_theme.text, DT_CENTER);

    // chords lane: the chord items' chords, edited in the arrange view
    {
        const Rect ch = chordsArea();
        cv.clip = {0, ch.t, KB_W, ch.b};
        cv.fill(0, ch.t, KB_W, ch.b, g_theme.button);
        text(dc, "Chords", 0, ch.t, KB_W, ch.b, g_theme.dim, DT_CENTER);
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
    cv.clip = {n.l, 0, n.r, n.b};
    double selA = 0, selB = 0;
    GetSet_LoopTimeRange2(NULL, false, false, &selA, &selB, false);
    if (selB > selA) {
        const int xa = (int)lround(g_ui.view.x(selA)), xb = (int)lround(g_ui.view.x(selB));
        cv.fill(xa, 0, xb, n.b, 0xffffff, 22);
        cv.fill(xa, 0, xa + 1, n.b, 0xffffff, 90), cv.fill(xb, 0, xb + 1, n.b, 0xffffff, 90);
    }

    // cursors
    int x = (int)lround(g_ui.view.x(GetCursorPosition()));
    cv.fill(x, 0, x + 1, n.b, C_EDIT_CURSOR);
    if (GetPlayState() & 1) {
        x = (int)lround(g_ui.view.x(GetPlayPosition2()));
        cv.fill(x, 0, x + 1, n.b, C_PLAY_CURSOR);
    }

    // lyrics lane: the visible lyric line as text, each syllable at its note's start
    {
        const Rect ly = lyricsArea();
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
    {
        const Rect o = overviewArea();
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

    // status line: pitch, frequency, cents of the strongest nearby peak, time, bar.beat, level
    const int sy = n.b + LYRICS_H + OVERVIEW_H; // status line top
    cv.clip = {0, n.b, n.l, sy};
    cv.fill(0, n.b, n.l, sy, g_theme.button);
    cv.clip = {0, sy, c.r, c.b};
    cv.fill(0, sy, c.r, c.b, g_theme.ruler);
    std::string status;
    if (n.in(g_ui.mouseX, g_ui.mouseY)) {
        const double t = g_ui.view.t(g_ui.mouseX), p = g_ui.view.p(g_ui.mouseY);
        const int ip = (int)floor(p + 0.5);
        char name[16], tm[32], bb[32];
        pitchName(ip, name, sizeof(name));
        format_timestr_pos(t, tm, sizeof(tm), 0);
        format_timestr_pos(t, bb, sizeof(bb), 2);
        snprintf(buf, sizeof(buf), "%-4s %7.1f Hz   %s   %s", name, 440 * pow(2.0, (p - 69) / 12),
                 tm, bb);
        status = buf;
        const int f = (int)lround((t - g_session.audioPos) * g_analysis.spec.frameRate());
        if (f >= 0 && f < g_analysis.spec.frames) {
            const int b0 = std::max(0, (int)floor(g_analysis.spec.binOfPitch(ip - 1))),
                      b1 = std::min(g_analysis.spec.bins - 1,
                                    (int)ceil(g_analysis.spec.binOfPitch(ip + 1)));
            int best = -1;
            for (int b = b0; b <= b1; b++) {
                if (best < 0 || g_analysis.spec.at(f, b) > g_analysis.spec.at(f, best)) {
                    best = b;
                }
            }
            if (best >= 0) {
                const double peakPitch =
                    69 + 12 * log2(g_analysis.spec.p.fmin *
                                   pow(2.0, (double)best / g_analysis.spec.p.binsPerOctave) / 440);
                const int pp = (int)floor(peakPitch + 0.5);
                pitchName(pp, name, sizeof(name));
                snprintf(
                    buf, sizeof(buf), "   peak %s %+.0f c   %.0f dB", name, (peakPitch - pp) * 100,
                    20 * log10(std::max(
                             1e-10f,
                             g_analysis.spec.at(
                                 f, std::max(0, std::min(g_analysis.spec.bins - 1,
                                                         (int)lround(
                                                             g_analysis.spec.binOfPitch(p))))))));
                status += buf;
            }
        }
    }
    int hovered = -1;
    if (n.in(g_ui.mouseX, g_ui.mouseY) &&
        g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY, &hovered) != NoteEditor::NONE) {
        const Note &hn = g_session.ed.notes[hovered];
        double sung = 0;
        const int f0 = (int)lround((hn.start - g_session.audioPos) * g_analysis.spec.frameRate()),
                  f1 = (int)lround((hn.end - g_session.audioPos) * g_analysis.spec.frameRate());
        if (contourMedian(g_analysis.contour, f0, f1,
                          (int)lround(0.05 * g_analysis.spec.frameRate()), &sung)) {
            snprintf(buf, sizeof(buf), "   note sung %+.0f c", (sung - hn.pitch) * 100);
            status += buf;
        }
    }
    static const char *const SNAP_NAMES[] = {"off", "onsets", "grid"};
    if (const int pm = analysisProgress(); pm >= 0) {
        snprintf(buf, sizeof(buf), "analysing %d%%   snap: %s (G)", pm / 10,
                 SNAP_NAMES[g_settings.snapMode]);
        cv.clip = {0, 0, c.r, c.b};
        cv.fill(n.l, n.t, n.l + (n.r - n.l) * pm / 1000, n.t + 3, C_ACCENT);
        cv.clip = {0, sy, c.r, c.b};
    } else {
        const std::string &note =
            g_analysis.error.empty() ? g_analysis.estimateNote : g_analysis.error;
        snprintf(buf, sizeof(buf), "%s%ssnap: %s (G)", note.c_str(), note.empty() ? "" : "   ",
                 SNAP_NAMES[g_settings.snapMode]);
    }
    text(dc, status.c_str(), 6, sy, c.r - 120, c.b, g_theme.text, DT_LEFT);
    text(dc, buf, 6, sy, c.r - 6, c.b, g_theme.dim, DT_RIGHT);

    cv.clip = {0, 0, c.r, c.b};
    if (panelShown()) {
        drawPanel(cv, dc);
    }

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
