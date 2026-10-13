// The toolbar: one row of labels, segmented choices, toggles and buttons, laid out again on each
// paint, click and tooltip from the window's width.
#include "ui/toolbar.h"

#include <span>

#include "notes/commands.h"
#include "playback/isolate.h"
#include "ui/panel.h"
#include "ui/theme.h"

namespace {

// What an item is and does; segments of one group share a kind and differ by Item::arg.
enum Kind {
    LABEL,
    VOICE,
    SNAP,
    SUGGEST,
    SUGGEST_FROM,
    CONTOUR,
    GHOSTS,
    EXPLAIN,
    LISTEN,
    SPEED,
    ISOLATE,
    REMOVE,
    FOLLOW,
    HELP,
    PANEL
};

// One thing on the toolbar.
struct Item {
    Rect r; // window points
    Kind kind;
    int arg;           // the voice or segment index
    const char *label; // static strings
    const char *tip;   // shown on the status line
    bool first, last;  // ends of a segmented group
};

const std::array<const char *, WINDOW_VOICES> VOICE_LABELS = {"Lead", "2", "3", "4"};

// The width of s in points: 12 pt Helvetica, roughly.
int textWidth(const char *s) { return 7 * static_cast<int>(std::string_view(s).size()); }

// The items left to right (the help and panel buttons on the right); what doesn't fit is left out.
std::vector<Item> items(const Layout &L) {
    std::vector<Item> out;
    const int t = L.toolbar.t + 6;
    const int b = L.toolbar.b - 6;
    const int rightEdge = L.toolbar.r - 8 - 28 - 6 - 64 - 12; // room for ? and Panel
    int x = 10;

    // adds an item w wide at x; the gap after it is left out inside a segmented group
    auto add = [&](Kind k, int arg, const char *label, const char *tip, int w, bool first = true,
                   bool last = true) {
        if (x + w > rightEdge) {
            x = 1 << 20; // nothing more fits
            return;
        }
        out.push_back({{x, t, x + w, b}, k, arg, label, tip, first, last});
        x += w + (last ? 6 : 0);
    };
    auto gap = [&] { x += 10; }; // between groups
    // a segmented group: one item per label, touching
    auto segment = [&](Kind k, std::span<const char *const> labels,
                       std::span<const char *const> tips) {
        const int n = static_cast<int>(labels.size());
        for (int i = 0; i < n; i++) {
            add(k, i, labels[idx(i)], tips[idx(i)], textWidth(labels[idx(i)]) + 16, i == 0,
                i == n - 1);
        }
    };

    static const std::array<const char *, WINDOW_VOICES> VOICE_TIPS = {
        "Edit the lead (1)  ·  Shift-click: hide or show a voice",
        "Edit voice 2 (2)  ·  Shift-click: hide or show",
        "Edit voice 3 (3)  ·  Shift-click: hide or show",
        "Edit voice 4 (4)  ·  Shift-click: hide or show"
    };
    add(LABEL, 0, "VOICE", "", 40);
    for (int v = 0; v < WINDOW_VOICES; v++) {
        add(VOICE, v, VOICE_LABELS[idx(v)], VOICE_TIPS[idx(v)],
            textWidth(VOICE_LABELS[idx(v)]) + 30);
    }
    gap();

    static const char *const SNAPS[] = {"Off", "Onsets", "Grid"};
    static const char *const SNAP_TIPS[] = {
        "No snapping (G cycles)", "Snap to onsets and pitch changes within 30 ms (G)",
        "Snap to the tempo grid (G)"
    };
    add(LABEL, 0, "SNAP", "", 36);
    segment(SNAP, SNAPS, SNAP_TIPS);
    gap();

    add(SUGGEST, 0, "Suggestions", "Show suggested notes (N)  ·  A: accept all in view",
        textWidth("Suggestions") + 16);
    static const char *const FROM[] = {"Estimator", "Lead contour"};
    static const char *const FROM_TIPS[] = {
        "Suggestions from the note estimator, in voices",
        "Suggestions from the lead's pitch contour"
    };
    segment(SUGGEST_FROM, FROM, FROM_TIPS);
    gap();

    add(CONTOUR, 0, "Contour", "The sung pitch as a line (P)", textWidth("Contour") + 16);
    add(GHOSTS, 0, "Ghosts", "Outlines where each note's overtones should be (H)",
        textWidth("Ghosts") + 16);
    add(EXPLAIN, 0, "Explain", "Dim the energy drawn notes explain (X)", textWidth("Explain") + 16);
    gap();

    static const char *const HEAR[] = {"Both", "Audio", "Notes"};
    static const char *const HEAR_TIPS[] = {
        "Hear the audio and the notes", "Hear only the audio (B swaps)",
        "Hear only the notes (B swaps)"
    };
    add(LABEL, 0, "HEAR", "", 38);
    segment(LISTEN, HEAR, HEAR_TIPS);
    const char *isolate = g_settings.isolateAll ? "Isolate all" : "Isolate";
    add(ISOLATE, 0, isolate,
        "Hear only the edited voice's pitch and overtones (I)  ·  shift+I: all shown voices",
        textWidth(isolate) + 16);
    add(REMOVE, 0, "Remove notes",
        "Lower every drawn note's pitch and overtones, to hear what isn't notated yet (U)",
        textWidth("Remove notes") + 16);
    static const char *const SPEEDS[] = {"100%", "75%", "50%"};
    static const char *const SPEED_TIPS[] = {
        "Play at full speed", "Play at 75%, pitch kept", "Play at 50%, pitch kept"
    };
    segment(SPEED, SPEEDS, SPEED_TIPS);
    gap();

    add(FOLLOW, 0, "Follow", "Page the view along with playback (T)", textWidth("Follow") + 16);

    // right-aligned: Panel 64 wide, then ? 28 wide 6 to its left
    const int r = L.toolbar.r - 8;
    out.push_back({{r - 64, t, r, b}, PANEL, 0, "Panel", "Settings panel (S)", true, true});
    out.push_back({{r - 64 - 34, t, r - 70, b}, HELP, 0, "?", "Shortcuts (?)", true, true});
    return out;
}

// Whether the item shows as on: the chosen voice or segment, a toggle that is on.
bool isOn(const Item &it) {
    switch (it.kind) {
    case VOICE: return it.arg == g_session.voice;
    case SNAP: return it.arg == g_settings.snapMode;
    case SUGGEST: return g_settings.suggest;
    case SUGGEST_FROM: return it.arg == g_settings.suggestFrom;
    case CONTOUR: return g_settings.showContour;
    case GHOSTS: return g_settings.ghosts;
    case EXPLAIN: return g_settings.explain;
    case LISTEN: return it.arg == listenMode();
    case SPEED: return it.arg == speedIndex();
    case FOLLOW: return g_settings.followPlay;
    case ISOLATE: return isolateOn() && !g_settings.isolateRemove;
    case REMOVE: return isolateOn() && g_settings.isolateRemove;
    case HELP: return g_ui.help;
    case PANEL: return g_ui.panel;
    case LABEL: return false;
    }
    return false;
}

// The clickable item at (x, y), or null.
const Item *itemAt(int x, int y, std::span<const Item> all) {
    for (const Item &it : all) {
        if (it.kind != LABEL && it.r.in(x, y)) {
            return &it;
        }
    }
    return nullptr;
}

} // namespace

void drawToolbar(Canvas &cv, HDC dc, const Layout &L) {
    const Rect tb = L.toolbar;
    cv.clip = tb;
    cv.fill(tb.l, tb.t, tb.r, tb.b, g_theme.panel);
    cv.fill(tb.l, tb.b - 1, tb.r, tb.b, g_theme.separator);

    const std::vector<Item> all = items(L);
    const Item *hover = itemAt(g_ui.mouseX, g_ui.mouseY, all);
    for (const Item &it : all) {
        const Rect &r = it.r;
        const bool on = isOn(it);
        const bool hov = &it == hover;
        switch (it.kind) {
        case LABEL: text(dc, it.label, r.l, r.t, r.r, r.b, g_theme.dim, DT_LEFT); break;
        case VOICE: {
            // a swatch in the voice's colour, framed when edited, faded and struck when hidden
            const unsigned int col = g_theme.voices[idx(it.arg)];
            const bool hidden = g_ui.hiddenVoices >> it.arg & 1;
            cv.rounded(r.l, r.t, r.r, r.b, hov ? g_theme.track : g_theme.button);
            if (on) {
                cv.roundedFrame(r.l, r.t, r.r, r.b, col, 2);
            }
            const int cy = (r.t + r.b) / 2;
            cv.rounded(r.l + 8, cy - 4, r.l + 16, cy + 4, col, hidden ? 90 : 256);
            text(dc, it.label, r.l + 20, r.t, r.r - 4, r.b, hidden ? g_theme.dim : g_theme.text,
                 DT_LEFT);
            if (hidden) {
                cv.fill(r.l + 20, cy, r.r - 6, cy + 1, g_theme.dim);
            }
            break;
        }
        case SNAP:
        case SUGGEST_FROM:
        case LISTEN:
        case SPEED: {
            // segments touch, split by a 1-point line in the panel colour
            RGB fill = hov ? g_theme.track : g_theme.button;
            if (on) {
                fill = g_theme.on;
            }
            cv.fill(r.l, r.t, r.r, r.b, fill);
            if (!it.last) {
                cv.fill(r.r - 1, r.t, r.r, r.b, g_theme.panel);
            }
            text(dc, it.label, r.l, r.t, r.r, r.b, on ? g_theme.onText : g_theme.dim, DT_CENTER);
            break;
        }
        case SUGGEST:
        case CONTOUR:
        case GHOSTS:
        case EXPLAIN:
        case ISOLATE:
        case REMOVE:
        case FOLLOW:
        case HELP:
        case PANEL: // toggles and buttons
            if (on || hov) {
                cv.rounded(r.l, r.t, r.r, r.b, hov ? g_theme.track : g_theme.button);
            }
            // an on toggle is tinted with the theme's toolbar-on colour, like REAPER's own
            // toolbar, so it doesn't look like a segment that isn't chosen
            if (on) {
                cv.rounded(r.l, r.t, r.r, r.b, g_theme.on, 60);
            }
            text(dc, it.label, r.l, r.t, r.r, r.b, on ? g_theme.on : g_theme.dim, DT_CENTER);
        }
    }
}

void toolbarClick(int x, int y, int mods, const Layout &L) {
    const std::vector<Item> all = items(L);
    const Item *it = itemAt(x, y, all);
    if (!it) {
        return;
    }
    switch (it->kind) {
    case VOICE:
        if (mods & MOD_SHIFT) {
            if (it->arg != g_session.voice) {
                g_ui.hiddenVoices ^= 1u << it->arg;
            }
        } else {
            g_ui.hiddenVoices &= ~(1u << it->arg);
            selectVoice(it->arg);
        }
        break;
    case SNAP:
        g_settings.snapMode = it->arg;
        setupEditorSnap();
        break;
    case SUGGEST: g_settings.suggest = !g_settings.suggest; break;
    case SUGGEST_FROM: g_settings.suggestFrom = it->arg; break;
    case CONTOUR: g_settings.showContour = !g_settings.showContour; break;
    case GHOSTS: g_settings.ghosts = !g_settings.ghosts; break;
    case EXPLAIN: g_settings.explain = !g_settings.explain; break;
    case LISTEN: setListen(it->arg); break;
    case SPEED: setSpeed(it->arg); break;
    case FOLLOW: g_settings.followPlay = !g_settings.followPlay; break;
    case ISOLATE: toggleIsolate(false); break;
    case REMOVE: toggleIsolate(true); break;
    case HELP: g_ui.help = !g_ui.help; break;
    case PANEL: g_ui.panel = !g_ui.panel; break;
    case LABEL: break;
    }
}

std::string toolbarTip(int x, int y, const Layout &L) {
    const std::vector<Item> all = items(L);
    const Item *it = itemAt(x, y, all);
    return it ? it->tip : "";
}
