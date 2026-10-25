// Paints the ReaTrace window into a device-pixel backbuffer: the spectrogram layer, pitch rows
// and grid, notes and suggestions, contour and guides, keyboard, ruler, lanes, overview, status
// line, shortcut sheet, panel and toolbar. Layout is in points; drawing is in device pixels.
#include "ui/paint.h"

#include <span>

#include "analysis.h"
#include "dsp/parallel.h"
#include "index.h"
#include "notes/commands.h"
#include "notes/lyrics.h"
#include "ui/draw.h"
#include "ui/panel.h"
#include "ui/theme.h"
#include "ui/toolbar.h"

static const int GUIDE_THICKNESS = 3; // overtone hover marks, points

// With Explain, energy on drawn notes and their overtones is scaled by this.
static const double EXPLAIN_DIM = 0.25;

// gridLines stops after this many lines, so a far zoom-out can't flood the frame.
static const size_t MAX_GRID_LINES = 4000;

// Drawing happens in device pixels: g_scale per point (2 on Retina). Layout stays in points.
static HDC g_memdc; // the backbuffer, g_memw by g_memh device pixels
static int g_memw, g_memh, g_scale = 1;
static int g_fontScale; // the scale g_font and g_mono were made for
static HFONT g_font, g_mono;

// The rendered spectrogram for the note area, device pixels, and what it was rendered for.
static std::vector<ARGB> g_layer;
static bool g_layerValid;
struct LayerKey {
    double t0, pps, pitchTop, rowH; // the view
    double audioPos;                // frames are mapped to time through the item's position
    int w, h, version;              // device pixels; the analysis values' version
    int explain;                    // 0 off, else notes version * 32 + harmonics
    int explainDrag;                // explain on: the drag step (NoteEditor::moves), -1 if none
    bool operator==(const LayerKey &) const = default;
};
static LayerKey g_layerKey;

static int iround(double v) { return static_cast<int>(lround(v)); }

// Whether MIDI pitch p is a black key (bits of 0x54A: C#, D#, F#, G#, A#).
static bool isBlack(int p) { return (0x54A >> (((p % 12) + 12) % 12)) & 1; }

// Marks the pixels of the layer (w by h, rows from note area point n.t) within half a semitone of
// each of nt's first `harmonics` overtones, over the note's time span.
static void markExplained(std::span<char> dim, const Rect &n, int w, int h, int harmonics,
                          const Note &nt) {
    const double S = g_scale;
    const int xa = std::max(0, static_cast<int>(floor((g_ui.view.x(nt.start) - n.l) * S)));
    const int xb = std::min(w, static_cast<int>(ceil((g_ui.view.x(nt.end) - n.l) * S)));
    for (int hh = 1; hh <= harmonics && xa < xb; hh++) {
        const double ep = nt.pitch + 12 * log2(static_cast<double>(hh));
        // rows whose centre (y + 0.5) lies between the half-semitone boundaries
        const int ya =
            std::max(0, static_cast<int>(floor((g_ui.view.y(ep + 0.5) - n.t) * S - 0.5)) + 1);
        const int yb = std::min(h, static_cast<int>(ceil((g_ui.view.y(ep - 0.5) - n.t) * S - 0.5)));
        for (int y = ya; y < yb; y++) {
            std::ranges::fill(std::span(dim).subspan(idx(y, w, xa), idx(xb - xa)), 1);
        }
    }
}

// Renders the note area at device resolution: g_scale pixels per point, in column stripes on
// several threads. Each pixel is the strongest value over the frames and bins it covers.
// Black-key rows are shaded; with Explain, energy on drawn notes and their overtones is dimmed
// (marked once per note and overtone, not tested per pixel).
static void renderLayer(const Rect &n) {
    const double S = g_scale;
    const int w = (n.r - n.l) * g_scale;
    const int h = (n.b - n.t) * g_scale;

    std::vector<char> black(idx(h));
    for (int y = 0; y < h; y++) {
        black[idx(y)] = static_cast<char>(isBlack(g_ui.view.pitchAt(n.t + (y + 0.5) / S)));
    }
    g_layer.resize(idx(w) * idx(h));
    for (int y = 0; y < h; y++) {
        const ARGB bg =
            black[idx(y)] ? g_theme.shadeBlack(opaque(g_theme.area)) : opaque(g_theme.area);
        std::fill_n(&g_layer[idx(y) * idx(w)], w, bg);
    }
    const Spectrogram &s = *g_analysis.spec;
    if (!s.frames || g_analysis.val.size() != s.mag.size()) {
        return;
    }

    // bins covered by each pixel row; a row narrower than a bin takes the nearest one
    std::vector<int> rowLo(idx(h)), rowHi(idx(h));
    for (int y = 0; y < h; y++) {
        const double hi = s.binOfPitch(g_ui.view.p(n.t + y / S));
        const double lo = s.binOfPitch(g_ui.view.p(n.t + (y + 1) / S));
        int a = static_cast<int>(ceil(lo));
        int b = static_cast<int>(ceil(hi)) - 1;
        if (a > b) {
            a = b = static_cast<int>(lround(0.5 * (lo + hi)));
        }
        rowLo[idx(y)] = std::max(0, a);
        rowHi[idx(y)] = std::min(s.bins - 1, b);
    }

    // Explain: pixels whose row centre is within half a semitone of a sounding note's overtone
    std::vector<char> dim;
    if (g_settings.explain) {
        dim.assign(idx(w) * idx(h), 0);
        const int nh = iround(g_settings.harmonics);
        for (const Note &nt : g_session.ed.notes) {
            markExplained(dim, n, w, h, nh, nt);
        }
        for (int v = 0; v < WINDOW_VOICES; v++) {
            if (v == g_session.voice) {
                continue;
            }
            for (const Note &nt : g_session.others[idx(v)].notes) {
                markExplained(dim, n, w, h, nh, nt);
            }
        }
    }

    const int stripes =
        std::max(1, std::min(8, static_cast<int>(std::thread::hardware_concurrency())));
    const std::span<const unsigned char> val = g_analysis.val;
    const std::span<const unsigned int> lut = g_analysis.lut;
    std::atomic<bool> stop{false};
    parallel(stripes, stop, [&](int stripe) {
        std::vector<unsigned char> col(idx(s.bins)); // the column's strongest value per bin
        for (int x = stripe * w / stripes; x < (stripe + 1) * w / stripes; x++) {
            // frames covered by this pixel column; a column narrower than a frame takes the
            // nearest one
            const double fa = (g_ui.view.t(n.l + x / S) - g_session.audioPos) * s.frameRate();
            const double fb = (g_ui.view.t(n.l + (x + 1) / S) - g_session.audioPos) * s.frameRate();
            int f0 = static_cast<int>(ceil(fa));
            int f1 = static_cast<int>(ceil(fb)) - 1;
            if (f0 > f1) {
                f0 = f1 = static_cast<int>(lround(0.5 * (fa + fb)));
            }
            f0 = std::max(f0, 0);
            f1 = std::min(f1, s.frames - 1);
            if (f0 > f1) {
                continue;
            }

            std::ranges::copy(val.subspan(idx(f0) * idx(s.bins), idx(s.bins)), col.begin());
            for (int f = f0 + 1; f <= f1; f++) {
                const std::span<const unsigned char> v =
                    val.subspan(idx(f) * idx(s.bins), idx(s.bins));
                for (int b = 0; b < s.bins; b++) {
                    col[idx(b)] = std::max(col[idx(b)], v[idx(b)]);
                }
            }

            for (int y = 0; y < h; y++) {
                if (rowLo[idx(y)] > rowHi[idx(y)]) {
                    continue;
                }
                unsigned char m = 0;
                for (int k = rowLo[idx(y)]; k <= rowHi[idx(y)]; k++) {
                    m = std::max(m, col[idx(k)]);
                }
                const size_t at = idx(y) * idx(w) + idx(x);
                if (!dim.empty() && dim[at]) {
                    m = static_cast<unsigned char>(m * EXPLAIN_DIM);
                }
                const ARGB c = opaque(lut[m]);
                g_layer[at] = black[idx(y)] ? g_theme.shadeBlack(c) : c;
            }
        }
    });
}

// The overview: the whole item over the overview's width, vocal range E2..C6 over its height.
static const int OV_LOW = 40, OV_HIGH = 84; // MIDI pitches
// The rendered overview, device pixels, and what it was rendered for.
static std::vector<ARGB> g_ovLayer;
static int g_ovW, g_ovH, g_ovVersion = -1;
static double g_ovPos, g_ovLen; // the item's position and length, seconds

// The time (seconds) at window x (points) in the overview.
double overviewTime(int x) {
    const Rect o = overviewArea();
    return g_session.audioPos + g_session.audioLen * (x - o.l) / std::max(1, o.r - o.l);
}

// The overview x (points) of time t (seconds).
static double overviewX(const Rect &o, double t) {
    return o.l + (t - g_session.audioPos) / std::max(1e-9, g_session.audioLen) * (o.r - o.l);
}

// The overview y (points) of a MIDI pitch.
static double overviewY(const Rect &o, double pitch) {
    return o.b - 1 - (pitch - OV_LOW) / (OV_HIGH - OV_LOW) * (o.b - o.t - 2);
}

// Renders the overview layer, w by h device pixels: the strongest value over the frames and bins
// each pixel covers.
static void renderOverview(int w, int h) {
    g_ovLayer.assign(idx(w) * idx(h), opaque(g_theme.area));
    g_ovW = w;
    g_ovH = h;
    g_ovVersion = g_analysis.valVersion;
    g_ovPos = g_session.audioPos;
    g_ovLen = g_session.audioLen;
    const Spectrogram &s = *g_analysis.spec;
    if (!s.frames || g_analysis.val.size() != s.mag.size()) {
        return;
    }

    const double lo = s.binOfPitch(OV_LOW - 0.5);
    const double hi = s.binOfPitch(OV_HIGH + 0.5);
    const std::span<const unsigned char> val = g_analysis.val;
    const std::span<const unsigned int> lut = g_analysis.lut;
    std::vector<unsigned char> col(idx(s.bins));
    for (int x = 0; x < w; x++) {
        // at least one frame per column
        const int f0 = std::max(0, static_cast<int>(static_cast<double>(x) * s.frames / w));
        const int f1 =
            std::min(s.frames,
                     std::max(f0 + 1, static_cast<int>(static_cast<double>(x + 1) * s.frames / w)));
        std::fill(col.begin(), col.end(), 0);
        for (int f = f0; f < f1; f++) {
            const std::span<const unsigned char> v = val.subspan(idx(f) * idx(s.bins), idx(s.bins));
            for (int b = 0; b < s.bins; b++) {
                col[idx(b)] = std::max(col[idx(b)], v[idx(b)]);
            }
        }

        for (int y = 0; y < h; y++) {
            const int b0 = std::max(0, static_cast<int>(hi - (hi - lo) * (y + 1) / h));
            const int b1 = std::min(s.bins - 1, static_cast<int>(hi - (hi - lo) * y / h));
            unsigned char m = 0;
            for (int b = b0; b <= b1; b++) {
                m = std::max(m, col[idx(b)]);
            }
            g_ovLayer[idx(y, w, x)] = opaque(lut[m]);
        }
    }
}

// A tempo grid line in the note area.
struct GridLine {
    int x;       // window points
    int level;   // 0 = measure, 1 = beat, 2 = subdivision
    int measure; // 0-based
};

// The tempo grid lines across the note area: every grid step, or only measures when the grid
// is off (gridStepQN() <= 0).
static std::vector<GridLine> gridLines(const Rect &n) {
    std::vector<GridLine> out;
    const double qa = TimeMap2_timeToQN(NULL, g_ui.view.t(n.l));
    const double qb = TimeMap2_timeToQN(NULL, g_ui.view.t(n.r));
    const double step = gridStepQN();
    if (step > 0) {
        for (long long k = static_cast<long long>(ceil(qa / step));
             static_cast<double>(k) * step <= qb && out.size() < MAX_GRID_LINES; k++) {
            const double q = static_cast<double>(k) * step;
            double ms = 0;
            // nudged so a line on a measure start counts as that measure
            const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &ms, NULL);
            int level = 2;
            if (fabs(q - ms) < 1e-6) {
                level = 0;
            } else if (fabs(q - floor(q + 0.5)) < 1e-6) {
                level = 1;
            }
            out.push_back({iround(g_ui.view.x(TimeMap2_QNToTime(NULL, q))), level, m});
        }
        return out;
    }

    double ms = 0, me = 0;
    TimeMap_QNToMeasures(NULL, qa, &ms, &me);
    for (double q = ms; q <= qb && out.size() < MAX_GRID_LINES;) {
        double s2 = 0, e2 = 0;
        const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &s2, &e2);
        out.push_back({iround(g_ui.view.x(TimeMap2_QNToTime(NULL, s2))), 0, m});
        q = std::max(e2, q + 1e-3); // always move on, even if a measure has no length
    }
    return out;
}

// Draws one line of text in the rectangle (points), colour col (0xRRGGBB), DT_* flags.
void text(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col, int flags) {
    SetTextColor(dc, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
    RECT rc = {l * g_scale, t * g_scale, r * g_scale, b * g_scale};
    DrawText(dc, s, -1, &rc, flags | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
}

// "C4" for MIDI pitch 60.
static std::string pitchName(int p) {
    static const std::array<const char *, 12> NAMES = {"C",  "C#", "D",  "D#", "E",  "F",
                                                       "F#", "G",  "G#", "A",  "A#", "B"};
    return NAMES[idx(((p % 12) + 12) % 12)] + std::to_string(p / 12 - 1);
}

// --- overtone guides ------------------------------------------------------------------

// Strongest level (dB) within half a semitone of `pitch` between times a and b (seconds).
static double levelDb(double pitch, double a, double b) {
    const Spectrogram &s = *g_analysis.spec;
    if (!s.frames) {
        return -200;
    }
    const int f0 = std::max(0, static_cast<int>(floor((a - g_session.audioPos) * s.frameRate())));
    const int f1 =
        std::min(s.frames - 1, static_cast<int>(ceil((b - g_session.audioPos) * s.frameRate())));
    const int b0 = std::max(0, static_cast<int>(ceil(s.binOfPitch(pitch - 0.5))));
    const int b1 = std::min(s.bins - 1, static_cast<int>(floor(s.binOfPitch(pitch + 0.5))));
    float m = 0;
    for (int f = f0; f <= f1; f++) {
        for (int k = b0; k <= b1; k++) {
            m = std::max(m, s.at(f, k));
        }
    }
    return static_cast<double>(20 * log10(std::max(m, 1e-10f)));
}

// The pitch of the h-th overtone of pitch p (h = 1 is p itself).
static double overtonePitch(double p, int h) { return p + 12 * log2(static_cast<double>(h)); }

// Note ghosts: faint outlines where each note's overtones should be, fainter higher up.
static void drawGhosts(Canvas &cv) {
    if (!g_settings.ghosts) {
        return;
    }
    const int a = iround(g_settings.guideOpacity * 160);
    const int n = iround(g_settings.harmonics);
    for (const Note &nt : g_session.ed.notes) {
        const int x0 = iround(g_ui.view.x(nt.start));
        const int x1 = std::max(x0 + 2, iround(g_ui.view.x(nt.end)));
        if (x1 < cv.clip.l || x0 > cv.clip.r) {
            continue;
        }
        for (int h = 2; h <= n; h++) {
            const double y = g_ui.view.y(overtonePitch(nt.pitch, h));
            const double hh = std::max(1.0, g_ui.view.rowH * 0.3); // half height
            const int y0 = iround(y - hh);
            const int y1 = iround(y + hh) + 1;
            cv.roundedFrame(x0, y0, x1, y1, g_theme.voices[idx(noteVoice(nt))], 1,
                            a / (h - 1) + a / 2);
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

    // the pitch, the time span measured (seconds) and the marks' x span (points)
    int p0 = 0;
    double tA = 0, tB = 0;
    int xA = 0, xB = 0;
    if (dn >= 0) {
        const Note &nt = g_session.ed.notes[idx(dn)];
        p0 = nt.pitch;
        tA = nt.start;
        tB = nt.end;
        xA = iround(g_ui.view.x(tA));
        xB = std::max(xA + 40, iround(g_ui.view.x(tB)));
    } else {
        p0 = g_ui.view.pitchAt(g_ui.mouseY);
        tA = g_ui.view.t(g_ui.mouseX) - 0.03;
        tB = g_ui.view.t(g_ui.mouseX) + 0.03;
        xA = g_ui.mouseX - 70;
        xB = g_ui.mouseX + 70;
    }

    const int a = iround(g_settings.guideOpacity * 256);
    const int nh = iround(g_settings.harmonics);
    char buf[32];
    for (int h = 1; h <= nh; h++) {
        const int y = iround(g_ui.view.y(overtonePitch(p0, h)));
        if (y < n.t || y >= n.b) {
            continue;
        }
        const int t = GUIDE_THICKNESS + (h == 1 ? 1 : 0); // the fundamental is thicker and solid
        cv.fill(xA, y - t / 2, xB, y - t / 2 + t, g_theme.voices[idx(g_session.voice)],
                h == 1 ? 256 : a);
        snprintf(buf, sizeof(buf), "%d", h);
        text(dc, buf, xB + 3, y - 8, xB + 30, y + 8, g_theme.text, DT_LEFT);
    }

    // octave check, as dashed lines
    const double own = levelDb(p0, tA, tB);
    static const struct {
        int h;
        const char *label;
    } below[] = {{2, "8vb"}, {3, "12th"}, {4, "2 oct"}};
    for (const auto &c : below) {
        const double pc = overtonePitch(p0, 1) - 12 * log2(static_cast<double>(c.h));
        const int y = iround(g_ui.view.y(pc));
        if (y < n.t || y >= n.b || g_analysis.spec->binOfPitch(pc) < 0) {
            continue;
        }
        const double rel = levelDb(pc, tA, tB) - own;
        const bool strong = rel > -12; // within 12 dB of the pitch itself
        const RGB col = strong ? g_theme.warn : g_theme.tick;
        for (int x = xA; x < xB; x += 8) {
            cv.fill(x, y - GUIDE_THICKNESS / 2, x + 4, y - GUIDE_THICKNESS / 2 + GUIDE_THICKNESS,
                    col, strong ? 256 : a);
        }
        snprintf(buf, sizeof(buf), " %+.0f dB", rel);
        text(dc, (c.label + std::string(buf)).c_str(), xB + 3, y - 8, xB + 120, y + 8, col,
             DT_LEFT);
    }
}

// Suggested notes not yet accepted, as dashed boxes.
static void drawSuggestions(Canvas &cv, const Rect &n) {
    if (!g_settings.suggest) {
        return;
    }
    const std::vector<Segment> &sug = suggestions();
    const double opacity =
        g_settings.suggestFrom == FROM_REFERENCE ? g_settings.referenceOpacity : 1;
    const auto op = [&](int a) { return iround(a * opacity); };
    for (size_t i = 0; i < sug.size(); i++) {
        const Segment &sg = sug[i];
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        segmentRect(sg, &x0, &y0, &x1, &y1);
        if (x1 < n.l || x0 > n.r || suggestionHidden(i) || covered(i)) {
            continue;
        }

        // dashed in the colour of the voice A puts it in (not a note yet), on a dark outline and
        // a tint so it stands out from bright energy
        const int v = suggestionVoice(i);
        const unsigned int col = v >= 0 ? g_theme.voices[idx(v)] : g_theme.crowded;
        const int a = op(v >= 0 ? 256 : 180);
        const int t = y1 - y0 >= 8 ? 2 : 1; // dash thickness
        cv.rounded(x0, y0, x1, y1, col, op(v >= 0 ? 60 : 30));
        cv.roundedFrame(x0 - 1, y0 - 1, x1 + 1, y1 + 1, g_theme.shadow, 1, op(170));
        cv.roundedFrame(x0 + t, y0 + t, x1 - t, y1 - t, g_theme.shadow, 1, op(110));
        for (int x = x0 + 1; x < x1 - 1; x += 8) {
            cv.fill(x, y0, std::min(x + 5, x1 - 1), y0 + t, col, a);
            cv.fill(x, y1 - t, std::min(x + 5, x1 - 1), y1, col, a);
        }
        for (int y = y0 + 1; y < y1 - 1; y += 6) {
            cv.fill(x0, y, x0 + t, std::min(y + 4, y1 - 1), col, a);
            cv.fill(x1 - t, y, x1, std::min(y + 4, y1 - 1), col, a);
        }
    }
}

// The sung pitch as a yellow line with a dark edge, one pixel column at a time: each column spans
// the pitches over its frames, joined to the previous column.
static void drawContour(Canvas &cv, const Rect &n) {
    const std::vector<float> &contour = g_analysis.contour.pitch;
    if (!g_settings.showContour || contour.empty()) {
        return;
    }
    const int frames = static_cast<int>(contour.size());
    const double fr = g_analysis.spec->frameRate();

    // linear between voiced frames, the nearer voiced frame next to an unvoiced one, -1 if none
    auto pitchAt = [&](double f) -> double {
        const int a = static_cast<int>(floor(f));
        const int b = a + 1;
        if (a < 0 || b >= frames) {
            return -1;
        }
        const double pa = static_cast<double>(contour[idx(a)]);
        const double pb = static_cast<double>(contour[idx(b)]);
        if (pa < 0 || pb < 0) {
            const bool nearA = f - a < 0.5;
            if (pa >= 0 && nearA) {
                return pa;
            }
            if (pb >= 0 && !nearA) {
                return pb;
            }
            return -1;
        }
        return pa + (pb - pa) * (f - a);
    };

    double prev = -1; // where the previous column ended, -1 if unvoiced
    for (int x = n.l; x < n.r; x++) {
        const double fa = (g_ui.view.t(x) - g_session.audioPos) * fr;
        const double fb = (g_ui.view.t(x + 1) - g_session.audioPos) * fr;
        double lo = 1e9, hi = -1e9;
        auto add = [&](double p) {
            if (p >= 0) {
                lo = std::min(lo, p);
                hi = std::max(hi, p);
            }
        };
        add(pitchAt(fa));
        add(pitchAt(fb));
        for (int f = static_cast<int>(ceil(fa)); f < fb; f++) {
            add(f >= 0 && f < frames ? static_cast<double>(contour[idx(f)]) : -1);
        }
        if (hi < 0) {
            prev = -1;
            continue;
        }
        if (prev >= 0) {
            add(prev); // connect to the previous column
        }
        prev = pitchAt(fb) >= 0 ? pitchAt(fb) : hi;

        const int y0 = iround(g_ui.view.y(hi));
        const int y1 = iround(g_ui.view.y(lo));
        cv.fill(x, y0 - 2, x + 1, y1 + 3, g_theme.shadow, 150);
        cv.fill(x, y0 - 1, x + 1, y1 + 2, g_theme.contour);
    }
}

// The active voice's notes with lyrics, by start time.
static std::vector<int> lyricOrder() {
    std::vector<int> out;
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        const Note &nt = g_session.ed.notes[i];
        if (!nt.lyric.empty() && noteVoice(nt) == g_session.voice) {
            out.push_back(static_cast<int>(i));
        }
    }
    std::sort(out.begin(), out.end(), [](int a, int b) {
        return g_session.ed.notes[idx(a)].start < g_session.ed.notes[idx(b)].start;
    });
    return out;
}

// The note whose syllable is shown at x in the lyrics lane, -1 if none.
int lyricAt(int x) {
    const double t = g_ui.view.t(x);
    int best = -1;
    for (int i : lyricOrder()) {
        if (g_session.ed.notes[idx(i)].start <= t) {
            best = i;
        }
    }
    return best;
}

// --- status line, hints, empty states, shortcuts ---------------------------------------

// text() in the monospaced font.
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
    if (g_ui.intervals) {
        return "Intervals: 3, -3 (in the key), m3, M6, P5, aug4, dim5; 3,5 for several; "
               "t3: transpose  ·  Enter: add  ·  Esc: cancel";
    }
    const int x = g_ui.mouseX, y = g_ui.mouseY;
    if (g_ui.snapChip.in(x, y)) {
        return "Click: snap mode, grid size and triplets  ·  G: cycle the mode";
    }
    static const std::array<const char *, WINDOW_VOICES> VOICE_NAMES = {
        "Lead", "voice 2", "voice 3", "voice 4"
    };
    const std::string voice = VOICE_NAMES[idx(g_session.voice)];
    if (L.notes.in(x, y)) {
        int hit = -1;
        const NoteEditor::Hit h = g_session.ed.hitTest(g_ui.view, x, y, &hit);
        if (h == NoteEditor::LEFT || h == NoteEditor::RIGHT) {
            return "Drag: resize  ·  Alt: no snap";
        }
        if (h == NoteEditor::BODY) {
            return "Drag: move  ·  Cmd+drag: copy  ·  Shift: lock to time or pitch  ·  "
                   "Double-click or Enter: lyric  ·  K: split";
        }
        if (const int i = suggestionAt(x, y); i >= 0) {
            const int v = suggestionVoice(idx(i));
            if (v < 0) {
                return "Crowded out (more notes than voices)  ·  Click: add it to " + voice +
                       "  ·  Drag: draw";
            }
            return std::string("Click: accept into ") + VOICE_NAMES[idx(v)] +
                   "  ·  Drag: draw  ·  A: accept all in view";
        }
        return "Drag: draw a note in " + voice + "  ·  Shift+drag: select  ·  Alt: no snap";
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

// A small rounded label on the status line ending at `right`; returns its left edge. Its width
// is a guess from the label's length (about 7 points per character).
static int chip(Canvas &cv, HDC dc, int right, int t, int b, const char *label, unsigned int bg,
                unsigned int fg) {
    const int w = 12 + 7 * static_cast<int>(std::string_view(label).size());
    const int l = right - w;
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
    cv.fill(s.l, s.t, s.r, s.t + 1, g_theme.separator);

    // the columns, filled while the mouse is over the note area
    char buf[160];
    static const std::array COLS = {44, 84, 112, 64, 92, 76, 100}; // widths, points
    std::array<std::string, COLS.size()> col;
    const bool over = n.in(g_ui.mouseX, g_ui.mouseY);
    if (over) {
        const double t = g_ui.view.t(g_ui.mouseX);
        const double p = g_ui.view.p(g_ui.mouseY);
        const int ip = static_cast<int>(floor(p + 0.5));
        const Spectrogram &sp = *g_analysis.spec;
        col[0] = pitchName(ip);
        snprintf(buf, sizeof(buf), "%7.1f Hz", sp.p.a4 * pow(2.0, (p - 69) / 12));
        col[1] = buf;

        // the strongest bin within a semitone of the pitch, and the level at the pitch
        const int f = iround((t - g_session.audioPos) * sp.frameRate());
        if (f >= 0 && f < sp.frames) {
            const int b0 = std::max(0, static_cast<int>(floor(sp.binOfPitch(ip - 1))));
            const int b1 = std::min(sp.bins - 1, static_cast<int>(ceil(sp.binOfPitch(ip + 1))));
            int best = -1;
            for (int b = b0; b <= b1; b++) {
                if (best < 0 || sp.at(f, b) > sp.at(f, best)) {
                    best = b;
                }
            }
            if (best >= 0) {
                const double peakHz =
                    sp.p.fmin * pow(2.0, static_cast<double>(best) / sp.p.binsPerOctave);
                const double peak = 69 + 12 * log2(peakHz / sp.p.a4);
                const int pp = static_cast<int>(floor(peak + 0.5));
                snprintf(buf, sizeof(buf), "peak %-3s %+3.0fc", pitchName(pp).c_str(),
                         (peak - pp) * 100);
                col[2] = buf;
                const int bin = std::max(0, std::min(sp.bins - 1, iround(sp.binOfPitch(p))));
                snprintf(buf, sizeof(buf), "%4.0f dB",
                         static_cast<double>(20 * log10(std::max(1e-10f, sp.at(f, bin)))));
                col[3] = buf;
            }
        }

        format_timestr_pos(t, buf, sizeof(buf), 0); // time
        col[4] = buf;
        format_timestr_pos(t, buf, sizeof(buf), 2); // measures.beats
        col[5] = buf;

        // how far the hovered note's sung pitch (its contour's median) is from it
        int hovered = -1;
        if (g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY, &hovered) !=
            NoteEditor::NONE) {
            const Note &hn = g_session.ed.notes[idx(hovered)];
            double sung = 0;
            const int f0 = iround((hn.start - g_session.audioPos) * sp.frameRate());
            const int f1 = iround((hn.end - g_session.audioPos) * sp.frameRate());
            const int skip = iround(0.05 * sp.frameRate()); // the scoop into the note
            if (contourMedian(g_analysis.contour, f0, f1, skip, &sung)) {
                snprintf(buf, sizeof(buf), "sung %+.0fc", (sung - hn.pitch) * 100);
                col[6] = buf;
            }
        }
    }
    int x = 6;
    for (size_t i = 0; i < col.size(); i++) {
        const char *shown = col[i].c_str();
        if (col[i].empty()) {
            shown = i < 2 && !over ? "—" : "";
        }
        monoText(dc, shown, x, s.t, x + COLS[i] - 6, s.b, i == 6 ? g_theme.dim : g_theme.text,
                 DT_LEFT);
        x += COLS[i];
        cv.fill(x - 4, s.t + 5, x - 3, s.b - 5, g_theme.track);
    }

    // modes, right to left: progress or the analysis note, voice, snap
    static const std::array SNAP_NAMES = {"snap: off", "snap: onsets", "snap: grid"};
    std::string snapName = SNAP_NAMES[idx(std::clamp(g_settings.snapMode, 0, 2))];
    if (g_settings.snapMode == SNAP_GRID) {
        // the step as a note value, e.g. 1/16 or 1/8T
        const double step = gridStepQN();
        const double straight = g_settings.gridTriplets ? step * 1.5 : step;
        if (step <= 0) {
            snapName += " bar";
        } else {
            snprintf(buf, sizeof(buf), " 1/%d%s", iround(4 / straight),
                     g_settings.gridTriplets ? "T" : "");
            snapName += buf;
        }
        if (g_settings.gridDiv == 0) {
            snapName += " (zoom)";
        }
    }
    static const std::array<const char *, WINDOW_VOICES> VOICE_NAMES = {
        "Lead", "Voice 2", "Voice 3", "Voice 4"
    };
    int right = c.r - 8;
    if (const int pm = analysisProgress(); pm >= 0) { // per mille
        cv.fill(right - 60, s.t + 10, right, s.b - 10, g_theme.button);
        cv.fill(right - 60, s.t + 10, right - 60 + 60 * pm / 1000, s.b - 10, g_theme.accent);
        right -= 66;
        snprintf(buf, sizeof(buf), "analysing %d%%", pm / 10);
        text(dc, buf, right - 90, s.t, right, s.b, g_theme.dim, DT_RIGHT);
        right -= 96;
    } else {
        const std::string &note =
            g_analysis.error.empty() ? g_analysis.estimateNote : g_analysis.error;
        if (!note.empty()) {
            text(dc, note.c_str(), x, s.t, right, s.b, g_theme.warn, DT_RIGHT);
            right -= 8 + 6 * static_cast<int>(note.size());
        }
    }
    int root = 0, scale = 0;
    std::string scaleName;
    if (currentKey(&root, &scale, &scaleName)) {
        static const std::array<const char *, 12> KEYS = {"C",  "C#", "D",  "Eb", "E",  "F",
                                                          "F#", "G",  "Ab", "A",  "Bb", "B"};
        const std::string k = std::string("key: ") + KEYS[idx(((root % 12) + 12) % 12)] + " " +
                              (scaleName.empty() ? "scale" : scaleName);
        right = chip(cv, dc, right, s.t, s.b, k.c_str(), g_theme.button, g_theme.dim);
        right -= 6;
    }
    right = chip(cv, dc, right, s.t, s.b, VOICE_NAMES[idx(g_session.voice)],
                 g_theme.voices[idx(g_session.voice)], g_theme.onText);
    right -= 6;
    const int snapRight = right;
    right = chip(cv, dc, right, s.t, s.b, snapName.c_str(), g_theme.button, g_theme.dim);
    g_ui.snapChip = {right, s.t, snapRight, s.b};
    right -= 10;

    // the hint gets what is left between the columns and the chips
    if (right > x + 40) {
        text(dc, hoverHint(L).c_str(), x + 4, s.t, right, s.b, g_theme.dim,
             DT_LEFT | DT_END_ELLIPSIS);
    }
}

// What to do when there is nothing to see yet.
static void drawEmptyStates(Canvas &cv, HDC dc, const Rect &n) {
    cv.clip = n;
    const int cx = (n.l + n.r) / 2, cy = (n.t + n.b) / 2;
    auto banner = [&](const char *msg, int y) { // centred on cx, y
        const int w = 24 + 7 * static_cast<int>(std::string_view(msg).size());
        cv.rounded(cx - w / 2, y - 14, cx + w / 2, y + 14, g_theme.shadow, 150);
        text(dc, msg, cx - w / 2, y - 14, cx + w / 2, y + 14, g_theme.text, DT_CENTER);
    };
    char buf[300];
    if (!g_analysis.spec->frames) {
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

// The shortcut sheet over the window (?, Esc to close): the groups in three columns.
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
          {"Link to arrange view", "L"},
          {"Follow playback", "T"}}},
        {"Draw and edit",
         {{"Draw a note", "drag on empty space"},
          {"Copy while moving", "cmd+drag a note"},
          {"Add intervals, transpose", "shift+I"},
          {"Lock to time or pitch", "shift while moving"},
          {"No snapping", "alt"},
          {"Split, join", "K, J"},
          {"Copy, cut, paste, duplicate", "cmd+C, X, V, D"},
          {"Quantise selected", "Q"},
          {"Nudge time, pitch", "arrows"},
          {"Nudge an octave", "shift+up, down"},
          {"Delete", "Delete, right-click"},
          {"Clear lyrics", "shift+Delete"}}},
        {"Suggestions and voices",
         {{"Show suggestions", "N"},
          {"Accept all in view", "A"},
          {"Edit every shown voice", "§ (` on US keyboards)"},
          {"Edit voice", "1-4"},
          {"Move selected to voice", "cmd+1-4"},
          {"Contour, ghosts, explain", "P, H, X"},
          {"Chord tones", "C"},
          {"Snap mode", "G; click the snap chip for the grid"}}},
        {"Lyrics",
         {{"Type a syllable", "Enter, double-click"},
          {"Next, previous note", "Tab, Space, -; shift+Tab, Backspace when empty"},
          {"Paste lyrics", "cmd+V"}}},
        {"Listen",
         {{"Play, stop", "Space"},
          {"Loop the selection", "R"},
          {"Audio or notes only", "B"},
          {"Isolate the edited voice, all voices", "I, alt+I"},
          {"Duck the notated pitches", "U"}}},
        {"Display",
         {{"Brightness, contrast", "[ ], - ="},
          {"Resolution", ", ."},
          {"Settings panel, dock", "S, D"},
          {"Tracks: mute and solo", "M"}}},
    };
    const Rect c = L.client, n = L.notes;
    cv.clip = c;
    cv.fill(0, n.t, c.r, c.b, g_theme.scrim, 215);

    const int cols = 3;
    const int colW = std::min(300, (c.r - 80) / cols);
    const int x0 = (c.r - colW * cols) / 2;
    text(dc, "Shortcuts   (? or Esc to close)", x0, n.t + 14, x0 + colW * cols, n.t + 34,
         g_theme.text, DT_LEFT);
    // each row of groups starts below the tallest group of the row above
    int g = 0, rowTop = n.t + 46, rowBottom = rowTop;
    for (const Group &group : GROUPS) {
        if (g > 0 && g % cols == 0) {
            rowTop = rowBottom + 16;
        }
        const int gx = x0 + (g % cols) * colW;
        int y = rowTop;
        g++;
        text(dc, group.title, gx, y, gx + colW, y + 18, g_theme.accent, DT_LEFT);
        y += 20;
        for (const Row &r : group.rows) {
            text(dc, r.what, gx, y, gx + colW - 10, y + 16, g_theme.text, DT_LEFT);
            monoText(dc, r.keys, gx, y, gx + colW - 14, y + 16, g_theme.dim, DT_RIGHT);
            y += 17;
        }
        rowBottom = std::max(rowBottom, y);
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
        const Rect lr = {0, lane.r.t, KB_W, lane.r.b}; // the label
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
        cv.fill(0, lr.t, L.notes.r, lr.t + 1, g_theme.separator);
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

// The chord items' chords, from the ReaChords extension when it is installed (looked up on use:
// it may load after this one). Read again after each project change or switch.
struct ChordSpan {
    double start = 0, end = 0; // project seconds
    std::string name;
};
static const std::vector<ChordSpan> &projectChords() {
    static std::vector<ChordSpan> spans;
    // the change count alone could match another project's, after switching project tabs
    static ReaProject *lastProj = NULL;
    static int lastChange = -1;
    ReaProject *proj = EnumProjects(-1, NULL, 0);
    const int change = GetProjectStateChangeCount(proj);
    if (proj == lastProj && change == lastChange) {
        return spans;
    }
    lastProj = proj;
    lastChange = change;
    spans.clear();
    using GetChord = int (*)(int, double *, double *, char *, int);
    const auto get = reinterpret_cast<GetChord>(plugin_getapi("ReaChords_GetChord"));
    ChordSpan c;
    char name[256];
    for (int i = 0; get && get(i, &c.start, &c.end, name, sizeof(name)); i++) {
        c.name = name;
        spans.push_back(c);
    }
    return spans;
}

// --- the note area and the lanes --------------------------------------------------------

// The key's pitch classes (see currentKey) as a bit per class from C, and its root; 0 without
// one.
static unsigned scalePitchClasses(int *root) {
    int scale = 0;
    *root = 0;
    if (!currentKey(root, &scale, NULL)) {
        return 0;
    }
    const unsigned s = static_cast<unsigned>(scale) & 0xfff;
    const unsigned r = static_cast<unsigned>(*root % 12);
    return ((s << r) | (s >> (12 - r))) & 0xfff; // rotated from the root to C
}

// Rows in the key faintly lighter; with chord tones on, each chord's notes shaded under
// it, its root stronger.
static void drawKeyAndChords(Canvas &cv, const Rect &n, int pLo, int pHi) {
    cv.clip = n;
    int root = 0;
    if (const unsigned key = scalePitchClasses(&root)) {
        for (int p = pLo; p <= pHi; p++) {
            if (key >> ((p % 12 + 12) % 12) & 1) {
                cv.fill(n.l, iround(g_ui.view.y(p + 0.5)), n.r, iround(g_ui.view.y(p - 0.5)),
                        g_theme.grid, 10);
            }
        }
    }
    if (!g_settings.chordTones) {
        return;
    }
    for (const ChordSpan &span : projectChords()) {
        const int xa = iround(g_ui.view.x(span.start));
        const int xb = iround(g_ui.view.x(span.end));
        const std::optional<ChordTones> c = chordTones(span.name);
        if (!c || xb < n.l || xa > n.r) {
            continue;
        }
        for (int p = pLo; p <= pHi; p++) {
            const int rel = ((p - c->root) % 12 + 12) % 12;
            if (c->tones >> rel & 1) {
                cv.fill(xa, iround(g_ui.view.y(p + 0.5)), xb, iround(g_ui.view.y(p - 0.5)),
                        g_theme.accent, rel == 0 ? 70 : 38);
            }
        }
    }
}

// How strongly a grid line of this level (GridLine::level) shows, of 256.
static int gridAlpha(int level) {
    if (level == 0) {
        return 70;
    }
    return level == 1 ? 32 : 14;
}

// A note's box in window points: at least 2 wide and 3 tall, so short notes stay visible.
struct NoteBox {
    int x0, y0, x1, y1;
};
static NoteBox noteBox(const Note &nt) {
    const int x0 = iround(g_ui.view.x(nt.start));
    const int x1 = std::max(x0 + 2, iround(g_ui.view.x(nt.end)));
    const int y0 = iround(g_ui.view.y(nt.pitch + 0.5));
    const int y1 = std::max(y0 + 3, iround(g_ui.view.y(nt.pitch - 0.5)));
    return {x0, y0, x1, y1};
}

// Overlaps with a chord shorter than this (seconds) don't count, so a note starting just before a
// chord change isn't judged by the chord before it.
static const double CHORD_SLACK = 0.08;
// Where a note is outside the chord its fill is darkened this much (of 256), fading over FADE
// points into the parts inside the chord.
static const int OUT_ALPHA = 60, FADE = 10;

// With chord tones on: the parts of a note (seconds) where its pitch is outside the chord sounding
// then and isn't a slash chord's bass. Where there is no chord, or one that can't be read, it is
// inside.
static std::vector<std::pair<double, double>>
outsideChord(const Note &nt, std::span<const ChordSpan> spans,
             std::span<const std::optional<ChordTones>> tones) {
    std::vector<std::pair<double, double>> out;
    const int pc = (nt.pitch % 12 + 12) % 12;
    for (size_t i = 0; i < spans.size(); i++) {
        const std::optional<ChordTones> &c = tones[i];
        const double a = std::max(nt.start, spans[i].start), b = std::min(nt.end, spans[i].end);
        if (!c || b - a < CHORD_SLACK || c->tones >> ((pc - c->root + 12) % 12) & 1 ||
            pc == c->bass) {
            continue;
        }
        if (!out.empty() && out.back().second >= a - 1e-6) {
            out.back().second = b; // outside the next chord too: one part
        } else {
            out.emplace_back(a, b);
        }
    }
    return out;
}

// Other voices (dimmed, not editable), the edited voice's notes, their lyrics, and the marquee.
// Where notes are outside the chord (chord tones on) they get a light dot at the start, the edited
// voice's also a darker fill there.
static void drawNotes(Canvas &cv, HDC dc, const Rect &n) {
    std::span<const ChordSpan> spans;
    std::vector<std::optional<ChordTones>> tones;
    if (g_settings.chordTones) {
        spans = projectChords();
        for (const ChordSpan &c : spans) {
            tones.push_back(chordTones(c.name));
        }
    }
    const auto outside = [&](const Note &nt) { return outsideChord(nt, spans, tones); };
    const auto x = [](double t) { return iround(g_ui.view.x(t)); };
    const auto dot = [&](int x0, int y0) {
        cv.fill(x0 + 2, y0 + 2, x0 + 5, y0 + 5, g_theme.highlight);
    };

    if (g_ui.showOthers) {
        for (int v = 0; v < WINDOW_VOICES; v++) {
            if (g_ui.hiddenVoices >> v & 1) {
                continue;
            }
            for (const Note &nt : g_session.others[idx(v)].notes) {
                const auto [x0, y0, x1, y1] = noteBox(nt);
                cv.rounded(x0, y0, x1, y1, g_theme.voices[idx(v)], 70);
                cv.roundedFrame(x0, y0, x1, y1, g_theme.voices[idx(v)]);
                if (const auto out = outside(nt); !out.empty()) {
                    dot(std::max(x0, x(out[0].first)), y0);
                }
            }
        }
    }

    // solid with a darker edge; selected ones get a light outline, so lyrics stay readable
    const int drawing = g_session.ed.drawing();
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        const Note &nt = g_session.ed.notes[i];
        const unsigned int noteColour = g_theme.voices[idx(noteVoice(nt))];
        const auto [x0, y0, x1, y1] = noteBox(nt);
        const bool sel = nt.sel || static_cast<int>(i) == drawing;
        cv.rounded(x0, y0, x1, y1, noteColour, 245);
        const auto out = outside(nt);
        for (const auto &[a, b] : out) { // inside the frame, fading into the parts inside the chord
            const int xa = std::max(x0 + 1, x(a)), xb = std::min(x1 - 1, x(b));
            cv.fill(xa, y0 + 1, xb, y1 - 1, g_theme.shadow, OUT_ALPHA);
            for (int k = 1; k < FADE; k++) {
                const int alpha = OUT_ALPHA * (FADE - k) / FADE;
                if (xa - k > x0) {
                    cv.fill(xa - k, y0 + 1, xa - k + 1, y1 - 1, g_theme.shadow, alpha);
                }
                if (xb + k < x1 - 1) {
                    cv.fill(xb + k - 1, y0 + 1, xb + k, y1 - 1, g_theme.shadow, alpha);
                }
            }
        }
        cv.roundedFrame(x0, y0, x1, y1, mix(noteColour, g_theme.shadow, 0.55));
        if (!out.empty()) {
            dot(std::max(x0, x(out[0].first)), y0);
        }
        if (sel) {
            cv.roundedFrame(x0 - 1, y0 - 1, x1 + 1, y1 + 1, g_theme.outline, y1 - y0 > 6 ? 2 : 1);
        }
    }

    // mark where a voice's notes of one pitch and channel overlap (MIDI can't tell them apart)
    // with a line
    std::vector<std::pair<int, const Note *>> byStart; // with their voices
    for (const Note &nt : g_session.ed.notes) {
        byStart.push_back({noteVoice(nt), &nt});
    }
    std::ranges::sort(byStart, [](const auto &a, const auto &b) {
        return std::tie(a.first, a.second->chan, a.second->pitch, a.second->start) <
               std::tie(b.first, b.second->chan, b.second->pitch, b.second->start);
    });
    const Note *last = NULL; // of this voice, pitch and channel, the note reaching furthest so far
    int lastVoice = -1;
    for (const auto &[voice, nt] : byStart) {
        if (last && (lastVoice != voice || last->chan != nt->chan || last->pitch != nt->pitch)) {
            last = NULL;
        }
        if (last && last->end > nt->start) {
            const int y = noteBox(*nt).y0 - 3;
            const int xa = x(nt->start), xb = std::max(xa + 1, x(std::min(last->end, nt->end)));
            cv.fill(xa, y - 2, xb, y, g_theme.overlap);
        }
        if (!last || nt->end > last->end) {
            last = nt;
            lastVoice = voice;
        }
    }

    // lyrics: inside the note when it is tall enough, else just below it
    for (const Note &nt : g_session.ed.notes) {
        if (nt.lyric.empty()) {
            continue;
        }
        const int x0 = iround(g_ui.view.x(nt.start));
        const int x1 = std::max(x0 + 2, iround(g_ui.view.x(nt.end)));
        const int y0 = iround(g_ui.view.y(nt.pitch + 0.5));
        const int y1 = iround(g_ui.view.y(nt.pitch - 0.5)); // the real height, not the minimum
        const int l = std::max(x0 + 2, n.l);
        const int r = std::min(std::max(x1, x0 + 60), n.r); // below a note, room for a word
        if (r <= l) {
            continue;
        }
        if (y1 - y0 >= 12) {
            const unsigned int noteColour = g_theme.voices[idx(noteVoice(nt))];
            text(dc, nt.lyric.c_str(), l, std::max(y0, n.t), std::min(x1 - 1, n.r),
                 std::min(y1, n.b), mix(noteColour, g_theme.shadow, 0.85), DT_LEFT);
        } else if (y1 + 14 <= n.b) {
            text(dc, nt.lyric.c_str(), l, y1, r, y1 + 14, g_theme.highlight, DT_LEFT);
        }
    }

    if (const auto m = g_session.ed.marquee()) {
        const int x0 = static_cast<int>(m->x0), y0 = static_cast<int>(m->y0);
        const int x1 = static_cast<int>(m->x1), y1 = static_cast<int>(m->y1);
        cv.fill(x0, y0, x1, y1, g_theme.highlight, 30);
        cv.frame(x0, y0, x1, y1, g_theme.highlight);
    }
}

// The keyboard column left of the note area, pitches pLo..pHi; the hovered pitch is lit.
static void drawKeyboard(Canvas &cv, HDC dc, const Rect &n, int pLo, int pHi) {
    cv.clip = {0, n.t, n.l, n.b};
    const bool overNotes = n.in(g_ui.mouseX, g_ui.mouseY);
    const bool overKeys = g_ui.mouseX < n.l && g_ui.mouseY >= n.t && g_ui.mouseY < n.b;
    const int hoverPitch = overNotes || overKeys ? g_ui.view.pitchAt(g_ui.mouseY) : -1;
    int root = 0;
    const unsigned inKey = scalePitchClasses(&root);
    for (int p = pLo; p <= pHi; p++) {
        const int y0 = iround(g_ui.view.y(p + 0.5));
        const int y1 = iround(g_ui.view.y(p - 0.5));
        RGB key = isBlack(p) ? g_theme.keyBlack : g_theme.keyWhite;
        if (p == hoverPitch) {
            key = g_theme.accent;
        }
        cv.fill(0, y0, n.l, y1, key);
        cv.fill(0, y1 - 1, n.l, y1, g_theme.keyEdge);
        // the key: a mark on its keys, wider on the root
        const int pc = (p % 12 + 12) % 12;
        if (inKey >> pc & 1) {
            cv.fill(n.l - (pc == root % 12 ? 7 : 4), y0 + 1, n.l - 1, y1 - 1, g_theme.accent);
        }
        if (p % 12 == 0 && y1 - y0 >= 8) { // octave names on the Cs, when there is room
            text(dc, pitchName(p).c_str(), 2, y0, n.l - 4, y1, g_theme.keyText, DT_RIGHT);
        }
    }
    cv.fill(n.l - 1, n.t, n.l, n.b, g_theme.separator);
}

// The ruler under the toolbar: bars on top, seconds below, onsets as marks on the bottom edge.
static void drawRuler(Canvas &cv, HDC dc, const Layout &L, std::span<const GridLine> grid) {
    const Rect n = L.notes, ru = L.ruler;
    const int rt = ru.t;
    char buf[160];
    cv.clip = {0, rt, ru.r, ru.b};
    cv.fill(0, rt, ru.r, ru.b, g_theme.ruler);
    cv.fill(0, ru.b - 1, ru.r, ru.b, g_theme.separator);

    // measures and beats; measure numbers at least 36 points apart
    int lastLabel = -1000;
    for (const GridLine &g : grid) {
        if (g.level > 1) {
            continue;
        }
        cv.fill(g.x, rt + (g.level == 0 ? 0 : 8), g.x + 1, rt + 14, g_theme.tick);
        if (g.level == 0 && g.x - lastLabel >= 36) {
            snprintf(buf, sizeof(buf), "%d", g.measure + 1);
            text(dc, buf, g.x + 3, rt, g.x + 60, rt + 14, g_theme.text, DT_LEFT);
            lastLabel = g.x;
        }
    }

    // seconds: the smallest step whose labels are at least 70 points apart
    static const double SECOND_STEPS[] = {0.01, 0.02, 0.05, 0.1,  0.2,  0.5,   1.0,  2.0,
                                          5.0,  10.0, 15.0, 30.0, 60.0, 120.0, 300.0};
    double secStep = 0.01;
    for (double s : SECOND_STEPS) {
        secStep = s;
        if (secStep * g_ui.view.pps >= 70) {
            break;
        }
    }
    for (long long k = static_cast<long long>(ceil(g_ui.view.t(n.l) / secStep));
         static_cast<double>(k) * secStep <= g_ui.view.t(n.r); k++) {
        const double t = static_cast<double>(k) * secStep;
        const int x = iround(g_ui.view.x(t));
        cv.fill(x, rt + 22, x + 1, ru.b - 1, g_theme.tickMinor);
        format_timestr_pos(t, buf, sizeof(buf), 0);
        monoText(dc, buf, x + 3, rt + 14, x + 90, ru.b - 1, g_theme.dim, DT_LEFT);
    }

    for (int f : g_analysis.onsets) {
        const int ox = iround(g_ui.view.x(frameTime(f)));
        cv.fill(ox - 1, ru.b - 6, ox + 2, ru.b - 1, g_theme.warn);
    }

    // the corner above the keyboard
    cv.clip = {0, rt, KB_W, ru.b};
    cv.fill(0, rt, KB_W, ru.b, g_theme.button);
}

// The chords lane: the chord items' chords (from the ReaChords extension), edited in the arrange
// view.
static void drawChordsLane(Canvas &cv, HDC dc, const Rect &ch) {
    cv.clip = ch;
    cv.fill(ch.l, ch.t, ch.r, ch.b, g_theme.ruler);
    for (const ChordSpan &span : projectChords()) {
        const int xa = iround(g_ui.view.x(span.start));
        const int xb = iround(g_ui.view.x(span.end));
        if (xb < ch.l || xa > ch.r) {
            continue;
        }
        cv.fill(xa, ch.t, xa + 1, ch.b, g_theme.tick);
        text(dc, span.name.c_str(), std::max(xa + 3, ch.l), ch.t, std::min(xb - 1, ch.r), ch.b,
             g_theme.text, DT_LEFT);
    }
    cv.fill(ch.l, ch.b - 1, ch.r, ch.b, g_theme.separator);
}

// The time selection over the ruler and the note area, then the edit and play cursors.
static void drawSelectionAndCursors(Canvas &cv, const Layout &L) {
    const Rect n = L.notes, ru = L.ruler;
    cv.clip = {n.l, ru.t, n.r, n.b};
    double selA = 0, selB = 0;
    GetSet_LoopTimeRange2(NULL, false, false, &selA, &selB, false);
    if (selB > selA) {
        const int xa = iround(g_ui.view.x(selA));
        const int xb = iround(g_ui.view.x(selB));
        cv.fill(xa, ru.t, xb, n.b, g_theme.highlight, 22);
        cv.fill(xa, ru.t, xa + 1, n.b, g_theme.highlight, 90);
        cv.fill(xb, ru.t, xb + 1, n.b, g_theme.highlight, 90);
    }

    const int edit = iround(g_ui.view.x(GetCursorPosition()));
    cv.fill(edit, ru.t, edit + 1, n.b, g_theme.editCursor);
    if (GetPlayState() & 1) { // playing
        const int play = iround(g_ui.view.x(GetPlayPosition()));
        cv.fill(play, ru.t, play + 1, n.b, g_theme.playCursor);
    }
}

// The lyrics lane: the visible lyric line as text, each syllable at its note's start and clipped
// at the next one.
static void drawLyricsLane(Canvas &cv, HDC dc, const Rect &ly) {
    const unsigned int noteColour = g_theme.voices[idx(g_session.voice)];
    cv.clip = ly;
    cv.fill(ly.l, ly.t, ly.r, ly.b, g_theme.ruler);
    const std::vector<int> order = lyricOrder();
    for (size_t k = 0; k < order.size(); k++) {
        const Note &nt = g_session.ed.notes[idx(order[k])];
        const int sx = iround(g_ui.view.x(nt.start));
        int next = ly.r;
        if (k + 1 < order.size()) {
            next = iround(g_ui.view.x(g_session.ed.notes[idx(order[k + 1])].start));
        }
        if (next < ly.l || sx > ly.r) {
            continue;
        }
        cv.fill(sx, ly.t, sx + 1, ly.t + 4, noteColour);
        text(dc, nt.lyric.c_str(), std::max(sx + 2, ly.l), ly.t, std::min(next - 1, ly.r), ly.b,
             nt.sel ? g_theme.highlight : g_theme.text, DT_LEFT);
    }
}

// The overview lane: the whole item's spectrogram (rendered when it changes), the notes, and the
// visible range framed with the rest darkened. fb is the w-pixel-wide backbuffer.
static void drawOverview(Canvas &cv, std::span<ARGB> fb, int w, const Layout &L) {
    const Rect n = L.notes, o = L.overview;
    const int S = g_scale;
    const int ow = (o.r - o.l) * S, oh = (o.b - o.t) * S;
    if (ow != g_ovW || oh != g_ovH || g_ovVersion != g_analysis.valVersion ||
        g_ovPos != g_session.audioPos || g_ovLen != g_session.audioLen) {
        renderOverview(ow, oh);
    }
    for (int y = 0; y < oh; y++) {
        std::ranges::copy(
            std::span(g_ovLayer).subspan(idx(y) * idx(ow), idx(ow)),
            fb.subspan(idx(o.t * S + y) * idx(w) + idx(o.l) * idx(S), idx(ow)).begin());
    }

    cv.clip = o;
    for (const Note &nt : g_session.ed.notes) {
        const int y = iround(overviewY(o, nt.pitch));
        const int xa = iround(overviewX(o, nt.start));
        const int xb = std::max(xa + 1, iround(overviewX(o, nt.end)));
        cv.fill(xa, y - 1, xb, y + 1, g_theme.voices[idx(noteVoice(nt))]);
    }

    const int va = iround(overviewX(o, g_ui.view.t(n.l)));
    const int vb = std::max(va + 2, iround(overviewX(o, g_ui.view.t(n.r))));
    cv.fill(o.l, o.t, va, o.b, g_theme.shadow, 120);
    cv.fill(vb, o.t, o.r, o.b, g_theme.shadow, 120);
    cv.frame(va, o.t, vb, o.b, g_theme.highlight);
    cv.fill(o.l, o.t, o.r, o.t + 1, g_theme.separator);
}

// --- the frame --------------------------------------------------------------------------

// Makes the backbuffer (w by h device pixels) and the fonts for device scale S, when they changed.
static void ensureBackbuffer(HDC hdc, int w, int h, int S) {
    if (g_memdc && w == g_memw && h == g_memh && g_fontScale == S) {
        return;
    }
    if (g_memdc) {
        SWELL_DeleteGfxContext(g_memdc);
    }
    g_memdc = SWELL_CreateMemContext(hdc, w, h);
    g_memw = w;
    g_memh = h;
    if (g_font && g_fontScale != S) {
        DeleteObject(g_font);
        g_font = NULL;
    }
    if (g_mono && g_fontScale != S) {
        DeleteObject(g_mono);
        g_mono = NULL;
    }
    if (!g_font) {
        g_font = CreateFont(12 * S, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH,
                            "Helvetica");
    }
    if (!g_mono) { // figures that don't shift as the mouse moves
        g_mono = CreateFont(11 * S, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH,
                            "Menlo");
    }
    g_fontScale = S;
    if (g_memdc && g_font) {
        SelectObject(g_memdc, g_font);
    }
}

void paint(HDC hdc) {
    const Layout L = layout(); // once per frame, so every area agrees
    const Rect c = L.client;
    if (c.r < 2 || c.b < 2) {
        return;
    }
    applyTheme();
    g_scale = SWELL_IsRetinaHWND(g_ui.hwnd) ? 2 : 1;
    const int S = g_scale;
    const int w = (c.r * S + 3) & ~3; // SWELL backing stores are a multiple of 4 wide
    const int h = c.b * S;
    ensureBackbuffer(hdc, w, h, S);
    ARGB *bits = g_memdc ? static_cast<ARGB *>(SWELL_GetCtxFrameBuffer(g_memdc)) : NULL;
    if (!bits) {
        return;
    }
    // SWELL hands out the w by h backbuffer as a bare pointer.
#pragma clang unsafe_buffer_usage begin
    const std::span<ARGB> fb(bits, idx(w) * idx(h));
#pragma clang unsafe_buffer_usage end
    if (L.tooSmall) {
        std::ranges::fill(fb, opaque(g_theme.bg));
        StretchBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, c.r * S, c.b * S, SRCCOPY);
        return;
    }
    syncViewOrigin();

    const Rect n = L.notes;
    Canvas cv = {fb, w, h, {0, 0, c.r, c.b}, S};
    // the background around the note area (the spectrogram layer covers that)
    cv.fill(0, 0, c.r, n.t, g_theme.bg);
    cv.fill(0, n.b, c.r, c.b, g_theme.bg);
    cv.fill(0, n.t, n.l, n.b, g_theme.bg);
    cv.fill(n.r, n.t, c.r, n.b, g_theme.bg);
    HDC dc = g_memdc;
    SetBkMode(dc, TRANSPARENT);

    // spectrogram, rendered again only when what it shows changed
    // explain dims around the notes: also as they are dragged (not committed yet), each step, and
    // back when the drag is cancelled
    const int explainKey =
        g_settings.explain ? g_session.notesVersion * 32 + iround(g_settings.harmonics) : 0;
    const int explainDrag = g_settings.explain && g_session.ed.active() ? g_session.ed.moves : -1;
    const LayerKey key = {g_ui.view.t0,    g_ui.view.pps,         g_ui.view.pitchTop,
                          g_ui.view.rowH,  g_session.audioPos,    (n.r - n.l) * S,
                          (n.b - n.t) * S, g_analysis.valVersion, explainKey,
                          explainDrag};
    if (!g_layerValid || key != g_layerKey) {
        renderLayer(n);
        g_layerKey = key;
        g_layerValid = true;
    }
    const int lw = (n.r - n.l) * S;
    for (int y = n.t * S; y < n.b * S; y++) {
        std::ranges::copy(std::span(g_layer).subspan(idx(y - n.t * S) * idx(lw), idx(lw)),
                          fb.subspan(idx(y) * idx(w) + idx(n.l) * idx(S), idx(lw)).begin());
    }

    // pitch rows (octaves stronger), then the time grid
    cv.clip = n;
    const int pLo = static_cast<int>(floor(g_ui.view.p(n.b)));
    const int pHi = static_cast<int>(ceil(g_ui.view.p(n.t)));
    for (int p = pLo; p <= pHi; p++) {
        const int y = iround(g_ui.view.y(p - 0.5)); // boundary below pitch p
        cv.fill(n.l, y, n.r, y + 1, g_theme.grid, p % 12 == 0 ? 50 : 10);
    }
    const std::vector<GridLine> grid = gridLines(n);
    for (const GridLine &g : grid) {
        cv.fill(g.x, n.t, g.x + 1, n.b, g_theme.grid, gridAlpha(g.level));
    }

    drawKeyAndChords(cv, n, pLo, pHi);
    drawGhosts(cv);
    drawContour(cv, n);
    drawSuggestions(cv, n);
    drawHoverGuides(cv, dc, n);
    drawNotes(cv, dc, n);
    drawKeyboard(cv, dc, n, pLo, pHi);
    drawRuler(cv, dc, L, grid);
    if (!(g_ui.foldedLanes & LANE_CHORDS)) {
        drawChordsLane(cv, dc, L.chords);
    }
    drawSelectionAndCursors(cv, L);
    if (!(g_ui.foldedLanes & LANE_LYRICS)) {
        drawLyricsLane(cv, dc, L.lyrics);
    }
    if (!(g_ui.foldedLanes & LANE_OVERVIEW)) {
        drawOverview(cv, fb, w, L);
    }

    drawEmptyStates(cv, dc, n);
    drawStatus(cv, dc, L);
    drawLaneLabels(cv, dc, L);
    if (g_ui.help) {
        drawShortcuts(cv, dc, L);
    }

    cv.clip = {0, 0, c.r, c.b};
    syncPlayback(); // what the panel and the toolbar show
    if (L.panelShown) {
        drawPanel(cv, dc);
    }
    drawToolbar(cv, dc, L);

    // 2:1 is SWELL's retina blit
    StretchBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, c.r * S, c.b * S, SRCCOPY);
    if (g_ui.edit) {
        placeEdit(); // follow scrolling and zooming while typing a lyric
    }
}

void releasePaintCache() {
    if (g_memdc) {
        SWELL_DeleteGfxContext(g_memdc);
    }
    g_memdc = NULL;
    g_memw = g_memh = 0;
    g_layerValid = false;
}
