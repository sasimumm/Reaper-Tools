// ReaChords' chord items: a custom PCM source that stores a chord progression, paints it in its
// item in the arrange view, and lets the user drag chord changes and type chord names there.

#include "chords.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reaper_plugin_functions.h"
#include "shared/guard.h"
#undef min
#undef max

// Chords are stored in quarter notes from the source start, so they follow tempo changes. The
// source start's project time is the item position minus its (rate-scaled) start offset.

// The source type written to project files.
static const char *const SOURCE_TYPE = "REACHORDS";
static const char *const OLD_SOURCE_TYPE = "REATRACE_CHORDS"; // from when this was part of ReaTrace

// A LICE pixel (0xAARRGGBB) from 0..255 components.
static LICE_pixel rgba(int r, int g, int b, int a = 255) {
    return static_cast<LICE_pixel>((b & 0xff) | ((g & 0xff) << 8) | ((r & 0xff) << 16) |
                                   ((a & 0xff) << 24));
}

// The chord item's colours.
static const LICE_pixel C_CHORD = rgba(50, 58, 75);         // chord bands alternate between
static const LICE_pixel C_CHORD_ALT = rgba(60, 70, 90);     // these two
static const LICE_pixel C_CHORD_LINE = rgba(200, 200, 220); // the line at each chord's start
static const LICE_pixel C_CHORD_TEXT = rgba(240, 240, 240); // chord names

// Defined with the chord edit field below; the inline editor calls it on a double-click.
static void beginEditAtClick(MediaItem *item, double clickTime,
                             const REAPER_inline_positioninfo *pi);

// strtod on s: the number and how many characters it took (0 if none).
static std::pair<double, size_t> parseNumber(const std::string &s) {
    char *end = NULL;
    // strtod only takes a C string and hands back a pointer into it.
#pragma clang unsafe_buffer_usage begin
    const double v = strtod(s.c_str(), &end);
    return {v, static_cast<size_t>(end - s.c_str())};
#pragma clang unsafe_buffer_usage end
}

// One chord change: the chord sounds from qn until the next chord (or the item's end).
struct Chord {
    double qn; // quarter notes from the source start
    std::string name;
};

// Where a chord item sits in the project.
struct Placement {
    double qn0 = 0;            // project QN of the source start
    double start = 0, end = 0; // item start and end, project seconds
};

// The chord item's media source. It plays silence; REAPER calls Extended() to paint the item and
// pass it mouse input (the "inline editor"). REAPER owns instances once they are set on a take.
class ChordSource : public PCM_source {
  public:
    std::vector<Chord> chords; // sorted by qn
    double length = 3600;      // seconds; the longest a chord item can be
    MediaItem *m_item = NULL;  // the item playing this source, if known; see item()

    // PCM_source interface. Every entry point REAPER calls into runs under guarded().

    PCM_source *Duplicate() override {
        return guarded("chord item copy", static_cast<PCM_source *>(nullptr), [&] {
            auto *d = new ChordSource;
            d->chords = chords;
            d->length = length;
            return static_cast<PCM_source *>(d);
        });
    }

    bool IsAvailable() override { return true; }
    const char *GetType() override { return SOURCE_TYPE; }
    bool SetFileName(const char *) override { return false; }
    int GetNumChannels() override { return 1; }
    double GetSampleRate() override { return 0; } // below 1: silent
    double GetLength() override { return length; }
    int PropertiesWindow(HWND) override { return 0; }

    // Silence, in case REAPER asks for samples anyway.
    void GetSamples(PCM_source_transfer_t *block) override {
        if (block->samples && block->length > 0) {
            std::fill_n(block->samples,
                        static_cast<size_t>(block->length) * static_cast<size_t>(block->nch),
                        ReaSample(0));
        }
        block->samples_out = block->length;
    }

    void GetPeakInfo(PCM_source_peaktransfer_t *block) override { block->peaks_out = 0; }

    void SaveState(ProjectStateContext *ctx) override {
        guarded("saving a chord item", [&] { saveState(ctx); });
    }

    int LoadState(const char *firstLine, ProjectStateContext *ctx) override {
        return guarded("loading a chord item", -1, [&] { return loadState(firstLine, ctx); });
    }

    int Extended(int call, void *parm1, void *parm2, void *parm3) override {
        return guarded("chord item", 0, [&] { return extended(call, parm1, parm2, parm3); });
    }

    // The source's block in the project file: "LENGTH <seconds>", then "BEAT <qn> <name>" per
    // chord.
    void saveState(ProjectStateContext *ctx) {
        ctx->AddLine("LENGTH %.9f", length);
        for (const Chord &c : chords) {
            ctx->AddLine("BEAT %.9f %s", c.qn, c.name.c_str());
        }
    }

    // Reads what saveState() wrote, up to the block's closing ">".
    int loadState(const char *, ProjectStateContext *ctx) {
        static constexpr std::string_view LENGTH = "LENGTH ", BEAT = "BEAT ";
        chords.clear();
        char line[4096];
        while (ctx->GetLine(line, sizeof(line)) == 0) {
            std::string_view s(line);
            s.remove_prefix(std::min(s.find_first_not_of(" \t"), s.size()));
            if (s.starts_with('>')) {
                break;
            }

            if (s.starts_with(LENGTH)) {
                length = parseNumber(std::string(s.substr(LENGTH.size()))).first;
            } else if (s.starts_with(BEAT)) {
                const std::string rest(s.substr(BEAT.size()));
                const auto [qn, used] = parseNumber(rest);
                if (used < rest.size() && rest[used] == ' ') {
                    chords.push_back({qn, rest.substr(used + 1)});
                }
            }
        }

        std::sort(chords.begin(), chords.end(),
                  [](const Chord &a, const Chord &b) { return a.qn < b.qn; });
        return 0;
    }

    void Peaks_Clear(bool) override {}
    int PeaksBuild_Begin() override { return 0; }
    int PeaksBuild_Run() override { return 0; }
    void PeaksBuild_Finish() override {}

    // REAPER's extension calls: which item plays this source, and the inline editor.
    int extended(int call, void *parm1, void *parm2, void *parm3) {
        if (call == PCM_SOURCE_EXT_SETITEMCONTEXT) {
            m_item = static_cast<MediaItem *>(parm1);
            return 1;
        }
        if (call == PCM_SOURCE_EXT_INLINEEDITOR) {
            return inlineEditor(static_cast<int>(reinterpret_cast<INT_PTR>(parm1)), parm2,
                                static_cast<REAPER_inline_positioninfo *>(parm3));
        }
        return 0;
    }

    // The item playing this source: REAPER says so with SETITEMCONTEXT; the scan is a fallback.
    MediaItem *item() {
        if (m_item && ValidatePtr2(NULL, m_item, "MediaItem*")) {
            return m_item;
        }
        m_item = NULL;
        for (int i = 0; i < CountMediaItems(NULL); i++) {
            MediaItem *it = GetMediaItem(NULL, i);
            MediaItem_Take *tk = GetActiveTake(it);
            if (tk && GetMediaItemTake_Source(tk) == this) {
                return m_item = it;
            }
        }
        return NULL;
    }

    // Where the item playing this source is; false if there is none (e.g. not on a take yet).
    bool placement(Placement *p) {
        MediaItem *it = item();
        MediaItem_Take *tk = it ? GetActiveTake(it) : NULL;
        if (!tk) {
            return false;
        }
        double rate = GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE");
        if (rate <= 0) {
            rate = 1;
        }

        p->start = GetMediaItemInfo_Value(it, "D_POSITION");
        p->end = p->start + GetMediaItemInfo_Value(it, "D_LENGTH");
        // the start offset is in source seconds; divided by the rate it is project seconds
        const double sourceStart = p->start - GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS") / rate;
        p->qn0 = TimeMap2_timeToQN(NULL, sourceStart);
        return true;
    }

    // Project seconds of qn (quarter notes from the source start).
    double timeOf(const Placement &p, double qn) const {
        return TimeMap2_QNToTime(NULL, p.qn0 + qn);
    }

    // Index of the chord starting at qn, or -1.
    int chordAt(double qn) const {
        for (size_t i = 0; i < chords.size(); i++) {
            if (fabs(chords[i].qn - qn) < 1e-6) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Sets, renames or (with an empty name) deletes the chord starting at qn. False if nothing
    // changed.
    bool setChord(double qn, const std::string &name) {
        const int i = chordAt(qn);
        if (i >= 0 && name.empty()) {
            chords.erase(chords.begin() + i);
        } else if (i >= 0 && chords[static_cast<size_t>(i)].name != name) {
            chords[static_cast<size_t>(i)].name = name;
        } else if (i < 0 && !name.empty()) {
            chords.insert(std::upper_bound(chords.begin(), chords.end(), qn,
                                           [](double v, const Chord &c) { return v < c.qn; }),
                          {qn, name});
        } else {
            return false;
        }
        return true;
    }

  private:
    int m_drag = -1;       // index of the chord whose start is being dragged
    double m_dragFrom = 0; // its start before the drag

    // The chord whose start is under the mouse (within GRAB_PX), ignoring ones within EDGE_PX of
    // the item's edges, where dragging resizes the item as usual. -1 if none.
    int boundaryAt(const REAPER_inline_positioninfo *pi) {
        static constexpr double GRAB_PX = 4, EDGE_PX = 6;
        Placement p;
        if (!placement(&p)) {
            return -1;
        }

        // item view x (pixels) of project time t
        const auto x = [&](double t) { return (t - pi->draw_start_time) * pi->pixels_per_second; };
        const double xs = x(p.start), xe = x(p.end);
        for (size_t i = 0; i < chords.size(); i++) {
            const double xc = x(timeOf(p, chords[i].qn));
            if (fabs(pi->mouse_x - xc) <= GRAB_PX && xc > xs + EDGE_PX && xc < xe - EDGE_PX) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Moves the dragged chord's start to the mouse (snapped), between its neighbours.
    void drag(const REAPER_inline_positioninfo *pi) {
        Placement p;
        if (!placement(&p)) {
            return;
        }

        const double t =
            SnapToGrid(NULL, pi->draw_start_time + pi->mouse_x / pi->pixels_per_second);

        // stay a little apart from the neighbours, so the order (and chordAt) holds
        const size_t i = static_cast<size_t>(m_drag);
        const double lo = i > 0 ? chords[i - 1].qn + 1e-3 : -1e9;
        const double hi = i + 1 < chords.size() ? chords[i + 1].qn - 1e-3 : 1e9;
        chords[i].qn = std::clamp(TimeMap2_timeToQN(NULL, t) - p.qn0, lo, hi);
    }

    // Ends a drag, with an undo point if the chord moved.
    void endDrag() {
        if (m_drag >= 0 && chords[static_cast<size_t>(m_drag)].qn != m_dragFrom) {
            Undo_OnStateChange("ReaChords: move chord change");
        }
        m_drag = -1;
        UpdateArrange();
    }

    // PCM_SOURCE_EXT_INLINEEDITOR: REAPER's window messages for the item's area of the arrange
    // view. msg 0 asks what we support; pi is the item view (mouse and drawing in its pixels).
    int inlineEditor(int msg, void *parm2, REAPER_inline_positioninfo *pi) {
        switch (msg) {
        case 0: return 1 | REAPER_INLINEFLAG_WANTOVERLAYEDCONTROLS;
        case WM_CREATE:
        case WM_DESTROY: return 1;
        case WM_PAINT: paint(static_cast<LICE_IBitmap *>(parm2), pi); return 1;
        case WM_SETCURSOR:
            if (!pi || (m_drag < 0 && boundaryAt(pi) < 0)) {
                return 0;
            }
            *static_cast<HCURSOR *>(pi->extraParms[0]) = LoadCursor(NULL, IDC_SIZEWE);
            return 1;
        case WM_LBUTTONDOWN:
            // on a chord change: drag it (capturing the mouse, so REAPER doesn't move the item)
            if (!pi || (m_drag = boundaryAt(pi)) < 0) {
                return 0;
            }
            m_dragFrom = chords[static_cast<size_t>(m_drag)].qn;
            return REAPER_INLINE_RETNOTIFY_SETCAPTURE | REAPER_INLINE_RETNOTIFY_INVALIDATE;
        case WM_MOUSEMOVE:
            if (m_drag < 0 || !pi) {
                return 0;
            }
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { // released without telling us
                endDrag();
            } else {
                drag(pi);
            }
            return REAPER_INLINE_RETNOTIFY_INVALIDATE;
        case WM_LBUTTONUP:
            if (m_drag < 0) {
                return 0;
            }
            endDrag();
            return REAPER_INLINE_RETNOTIFY_INVALIDATE;
        case WM_LBUTTONDBLCLK:
            // REAPER sends the second click of a double-click as this (and no button-up after it)
            if (!pi) {
                return 0;
            }
            beginEditAtClick(item(), pi->draw_start_time + pi->mouse_x / pi->pixels_per_second, pi);
            return REAPER_INLINE_RETNOTIFY_INVALIDATE;
        default: return 0;
        }
    }

    // Bold Helvetica in the given pixel size, created on first use.
    static LICE_IFont *font(int size) {
        static std::map<int, LICE_IFont *> fonts;
        LICE_IFont *&f = fonts[size];
        if (!f) {
            f = LICE_CreateFont();
            LICE__SetFromHFont(f,
                               CreateFont(size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          DEFAULT_QUALITY, DEFAULT_PITCH, "Helvetica"),
                               0);
            LICE__SetTextColor(f, C_CHORD_TEXT);
        }
        return f;
    }

    // The largest font that fits the name in w by h pixels: from 3/4 of h down in steps of an
    // eighth (at least 2 px), stopping at about 10 px even if it doesn't fit.
    static LICE_IFont *fittingFont(const std::string &name, int w, int h) {
        int size = std::clamp(static_cast<int>(lround(h * 0.75)), 10, 96);
        for (; size > 10; size -= std::max(2, size / 8)) {
            RECT r = {0, 0, 0, 0};
            LICE__DrawText(font(size), NULL, name.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE);
            if (r.right - r.left <= w) {
                break;
            }
        }
        return font(size);
    }

    // Each chord as a band with a line at its start and its name, in the item's bitmap.
    void paint(LICE_IBitmap *bm, const REAPER_inline_positioninfo *pi) {
        Placement p;
        if (!bm || !pi || !placement(&p)) {
            return;
        }

        const int h = pi->height, y0 = -pi->draw_start_y;
        // item view x (pixels) of project time t, clamped so it converts to int safely
        const auto x = [&](double t) {
            return std::clamp((t - pi->draw_start_time) * pi->pixels_per_second, -1e6, 1e6);
        };
        const auto xi = [&](double t) { return static_cast<int>(lround(x(t))); };

        for (size_t i = 0; i < chords.size(); i++) {
            const bool last = i + 1 >= chords.size();
            const int x0 = xi(timeOf(p, chords[i].qn));
            const int x1 = xi(last ? 1e9 : timeOf(p, chords[i + 1].qn)); // the last runs on
            if (x1 < 0 || x0 > pi->width) {
                continue;
            }

            LICE_FillRect(bm, x0, y0, x1 - x0, h, i % 2 ? C_CHORD_ALT : C_CHORD, 1.0f, 0);
            LICE_Line(bm, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x0),
                      static_cast<float>(y0 + h), C_CHORD_LINE, 1.0f, 0, false);
            // the name starts at the chord, or at the item's left edge for a chord started before
            const int l = std::max(x0, 0) + 6;
            const int r = std::min(x1, pi->width) - 4;
            RECT rc = {l, y0, std::max(l, r), y0 + h};
            LICE__DrawText(fittingFont(chords[i].name, r - l, h), bm, chords[i].name.c_str(), -1,
                           &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
};

// The item's chord source, or NULL if it isn't a (valid) chord item.
static ChordSource *sourceOf(MediaItem *item) {
    if (!item || !ValidatePtr2(NULL, item, "MediaItem*")) {
        return NULL;
    }
    MediaItem_Take *tk = GetActiveTake(item);
    PCM_source *src = tk ? GetMediaItemTake_Source(tk) : NULL;
    if (!src || std::string_view(src->GetType()) != SOURCE_TYPE) {
        return NULL;
    }
    return static_cast<ChordSource *>(src);
}

// --- typing a chord in the arrange view -------------------------------------------
//
// A native edit field over the chord's band, a child of the arrange view. It is placed from the
// click (cursor position minus the click's offset in the item gives the item view's origin), so
// it doesn't follow scrolling while open; scrolling just leaves it where it was.

// The open edit field, if any (one at a time).
static struct {
    HWND hwnd = NULL;
    MediaItem *item = NULL;
    double qn = 0;                 // the chord being typed, QN from the source start
    int originX = 0, originY = 0;  // item view's (0,0) in arrange client coordinates
    double drawStart = 0, pps = 1; // item view's mapping at the click
    int focusTries = 0;            // REAPER may take focus back after the click; retake it
} g_ce;

// The arrange view's window: control 1000 of REAPER's main window.
static HWND arrangeHwnd() { return GetDlgItem(GetMainHwnd(), 1000); }

// Opens the edit field for the chord at qn (QN from the source start) of the item, from the
// chord's start to the next chord's (60..240 px wide), using the mapping beginEditAtClick() saved.
static void beginEdit(MediaItem *item, double qn) {
    ChordSource *src = sourceOf(item);
    Placement p;
    if (!src || !src->placement(&p)) {
        return;
    }
    const int i = src->chordAt(qn);

    double next = p.end;
    for (const Chord &c : src->chords) {
        if (c.qn > qn + 1e-6) {
            next = std::min(next, src->timeOf(p, c.qn));
            break;
        }
    }

    // arrange view client x of project time t
    const auto x = [&](double t) { return g_ce.originX + (t - g_ce.drawStart) * g_ce.pps; };
    const double t = src->timeOf(p, qn);
    const int x0 = static_cast<int>(lround(x(t)));
    const int w = static_cast<int>(std::clamp(x(next) - x(t) - 2, 60.0, 240.0));
    g_ce.item = item;
    g_ce.qn = qn;

    // SWELL creates controls in the parent set here; it is reset afterwards
    if (!g_ce.hwnd) {
        SWELL_MakeSetCurParms(1, 1, 0, 0, arrangeHwnd(), false, false);
        g_ce.hwnd = SWELL_MakeEditField(1001, 0, 0, 90, 20, 0);
        SWELL_MakeSetCurParms(1, 1, 0, 0, NULL, false, false);
    }
    if (!g_ce.hwnd) {
        return;
    }

    SetWindowText(g_ce.hwnd, i >= 0 ? src->chords[static_cast<size_t>(i)].name.c_str() : "");
    SetWindowPos(g_ce.hwnd, NULL, x0 + 1, g_ce.originY + 1, w, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_ce.hwnd, SW_SHOW);
    SetFocus(g_ce.hwnd);
    SendMessage(g_ce.hwnd, EM_SETSEL, 0, -1);
    g_ce.focusTries = 5;
}

// Double-click: the chord whose label was clicked, or the one starting at the clicked grid
// position, else a new chord there.
static void beginEditAtClick(MediaItem *item, double clickTime,
                             const REAPER_inline_positioninfo *pi) {
    ChordSource *src = sourceOf(item);
    Placement p;
    if (!src || !src->placement(&p)) {
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(arrangeHwnd(), &pt);
    g_ce.originX = pt.x - pi->mouse_x;
    g_ce.originY = pt.y - pi->mouse_y - pi->draw_start_y;
    g_ce.drawStart = pi->draw_start_time;
    g_ce.pps = pi->pixels_per_second;

    // a later chord wins where labels overlap
    double qn = TimeMap2_timeToQN(NULL, SnapToGrid(NULL, clickTime)) - p.qn0;
    for (const Chord &c : src->chords) {
        const double t = src->timeOf(p, c.qn);
        if (clickTime >= t && clickTime < t + 60 / pi->pixels_per_second) { // ~60 px of label
            qn = c.qn;
        }
    }
    beginEdit(item, qn);
}

// Closes the edit field. commit: store the typed text (trimmed; empty deletes the chord), else
// drop it.
static void endEdit(bool commit) {
    if (!g_ce.hwnd) {
        return;
    }
    char buf[256] = "";
    GetWindowText(g_ce.hwnd, buf, sizeof(buf));
    DestroyWindow(g_ce.hwnd);
    g_ce.hwnd = NULL;
    SetFocus(arrangeHwnd());

    ChordSource *src = sourceOf(g_ce.item); // the item may have gone meanwhile
    if (!commit || !src) {
        return;
    }
    std::string name = buf;
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    if (src->setChord(g_ce.qn, name)) {
        Undo_OnStateChange("ReaChords: edit chord");
        UpdateArrange();
    }
}

// -1 passes the key on to the edit field.
int Chords_TranslateAccel(MSG *msg) {
    if (!g_ce.hwnd || msg->hwnd != g_ce.hwnd) {
        return 0;
    }
    // typing a chord: Enter done, Esc cancel; the rest goes to the field
    if (msg->message == WM_KEYDOWN) {
        const int key = static_cast<int>(msg->wParam);
        if (key == VK_RETURN) {
            endEdit(true);
            return 1;
        }
        if (key == VK_ESCAPE) {
            endEdit(false);
            return 1;
        }
    }
    return -1;
}

// Keeps the edit field focused for a few ticks after it opens, then commits it once it loses focus.
void Chords_Timer() {
    if (!g_ce.hwnd || GetFocus() == g_ce.hwnd) {
        g_ce.focusTries = 0;
        return;
    }
    if (g_ce.focusTries-- > 0) {
        SetFocus(g_ce.hwnd);
        return;
    }
    endEdit(true); // clicking elsewhere finishes the chord
}

// --- the project's chords, for other extensions (ReaTrace's chords lane) ---------------

const std::vector<ChordSpan> &Chords_Spans() {
    // all items are scanned again after each project change; fine for typical project sizes
    static std::vector<ChordSpan> spans;
    static int lastChange = -1;
    const int change = GetProjectStateChangeCount(NULL);
    if (change == lastChange) {
        return spans;
    }
    lastChange = change;
    spans.clear();
    for (int i = 0; i < CountMediaItems(NULL); i++) {
        MediaItem *it = GetMediaItem(NULL, i);
        ChordSource *src = sourceOf(it);
        if (src) {
            src->m_item = it; // known here: placement() needn't scan the project for it
        }
        Placement p;
        if (!src || !src->placement(&p)) {
            continue;
        }

        // each chord until the next one, clipped to the item
        for (size_t k = 0; k < src->chords.size(); k++) {
            const bool last = k + 1 >= src->chords.size();
            const double next = last ? p.end : src->timeOf(p, src->chords[k + 1].qn);
            const double a = std::max(p.start, src->timeOf(p, src->chords[k].qn));
            const double b = std::min(p.end, next);
            if (b > a) {
                spans.push_back({a, b, src->chords[k].name});
            }
        }
    }

    std::sort(spans.begin(), spans.end(),
              [](const ChordSpan &a, const ChordSpan &b) { return a.start < b.start; });
    return spans;
}

// --- registration --------------------------------------------------------------------

// REAPER's source factory: chord items only come from project files, by type, not from files.
static PCM_source *createFromType(const char *type, int) {
    return guarded("creating a chord item", static_cast<PCM_source *>(nullptr), [&] {
        const std::string_view t(type);
        if (t != SOURCE_TYPE && t != OLD_SOURCE_TYPE) {
            return static_cast<PCM_source *>(nullptr);
        }
        return static_cast<PCM_source *>(new ChordSource);
    });
}

static PCM_source *createFromFile(const char *, int) { return NULL; }

static const char *enumFileExtensions(int, const char **desc) {
    if (desc) {
        *desc = NULL;
    }
    return NULL;
}
static pcmsrc_register_t g_reg = {createFromType, createFromFile, enumFileExtensions};

static int g_cmdInsert; // command id of "ReaChords: Insert chord item"

// A chord item on the first selected track over the time selection (else 4 bars from the edit
// cursor), with a demo progression a chord per bar. Its timebase is beats, so it follows tempo.
static void insertItem() {
    MediaTrack *tr = GetSelectedTrack(NULL, 0);
    if (!tr) {
        ShowMessageBox("Select a track first.", "ReaChords", 0);
        return;
    }

    // the item's span [a, b) in project seconds; qa is its start in project QN
    double a = 0, b = 0;
    GetSet_LoopTimeRange2(NULL, false, false, &a, &b, false);
    const double qa = TimeMap2_timeToQN(NULL, b > a ? a : GetCursorPosition());
    double ms = 0, me = 0; // the measure at qa, in QN
    TimeMap_QNToMeasures(NULL, qa, &ms, &me);
    const double bar = me - ms;
    if (b <= a) {
        b = TimeMap2_QNToTime(NULL, qa + 4 * bar);
        a = TimeMap2_QNToTime(NULL, qa);
    }

    Undo_BeginBlock2(NULL);
    MediaItem *it = AddMediaItemToTrack(tr);
    SetMediaItemInfo_Value(it, "D_POSITION", a);
    SetMediaItemInfo_Value(it, "D_LENGTH", b - a);
    SetMediaItemInfo_Value(it, "B_LOOPSRC", 0);
    SetMediaItemInfo_Value(it, "C_BEATATTACHMODE", 1); // timebase beats (position, length, rate)

    MediaItem_Take *tk = AddTakeToMediaItem(it);
    auto *src = new ChordSource;
    const std::array<const char *, 4> demo = {"C", "G", "Am", "F"};
    const double qb = TimeMap2_timeToQN(NULL, b);
    for (int i = 0; i < 4 && qa + i * bar < qb - 1e-6; i++) { // only bars that start inside
        src->chords.push_back({i * bar, demo[static_cast<size_t>(i)]});
    }
    SetMediaItemTake_Source(tk, src); // the take owns src from here

    char name[] = "Chords";
    GetSetMediaItemTakeInfo_String(tk, "P_NAME", name, true);
    Undo_EndBlock2(NULL, "ReaChords: insert chord item", -1);
    UpdateArrange();
}

// Registers the source type and the insert action.
void Chords_Register(reaper_plugin_info_t *rec) {
    rec->Register("pcmsrc", &g_reg);
    custom_action_register_t a = {0, "REACHORDS_INSERT", "ReaChords: Insert chord item", NULL};
    g_cmdInsert = rec->Register("custom_action", &a);
}

bool Chords_OnAction(int command) {
    if (!g_cmdInsert || command != g_cmdInsert) {
        return false;
    }
    insertItem();
    return true;
}
