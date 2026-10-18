// The tracks panel: every project track, indented by folder, with mute and solo buttons that can
// be set several at a time by dragging over them, as on REAPER's own tracks, and on MIDI tracks a
// guide button making them the reference the suggestions come from.
#include "ui/tracks.h"

#include "index.h"
#include "notes/takesync.h"

static const int ROW_H = 24, BELOW = 8, MARGIN = 12, BUTTON_W = 22, BUTTON_GAP = 4;
static const int INDENT = 12;                            // points per folder level
static const RGB MUTE_ON = 0xd84a4a, SOLO_ON = 0xe0c040; // REAPER's red mute, yellow solo

enum Button { NO_BUTTON, GUIDE, MUTE, SOLO };
static Button g_drag = NO_BUTTON; // the kind of button pressed; the drag sets it to g_dragOn
static bool g_dragOn;

int tracksHeight() { return CountTracks(NULL) * ROW_H + BELOW; }

// The track row at client y, the rows starting at top; -1 if none.
static int rowAt(int y, int top) {
    const int dy = y - top;
    const int i = dy >= 0 ? dy / ROW_H : -1;
    return i < CountTracks(NULL) ? i : -1;
}

// The button's left and right edges, in client points: guide, mute, solo from the left.
static std::pair<int, int> buttonSpan(Button b) {
    const int r = panelArea().r - MARGIN - (SOLO - b) * (BUTTON_W + BUTTON_GAP);
    return {r - BUTTON_W, r};
}

// Whether track tr has button b (only MIDI tracks can guide).
static bool hasButton(MediaTrack *tr, Button b) { return b != GUIDE || canReference(tr); }

static bool isOn(MediaTrack *tr, Button b) {
    if (b == GUIDE) {
        return isReference(tr);
    }
    return GetMediaTrackInfo_Value(tr, b == MUTE ? "B_MUTE" : "I_SOLO") != 0;
}

// Mute and solo through the control surface calls, so REAPER's mixer, solo mode and undo follow.
static void setOn(MediaTrack *tr, Button b, bool on) {
    if (b == GUIDE) {
        setReference(tr, on);
    } else if (b == MUTE) {
        CSurf_OnMuteChange(tr, on);
    } else {
        CSurf_OnSoloChange(tr, on);
    }
}

bool tracksMouse(int x, int y, int top, bool down) {
    const int row = rowAt(y, top);
    if (down) {
        g_drag = NO_BUTTON;
        if (y < top || y >= top + tracksHeight()) {
            return false; // below the rows: the panel's sections
        }
        for (const Button b : {GUIDE, MUTE, SOLO}) {
            const auto [l, r] = buttonSpan(b);
            if (row >= 0 && x >= l && x < r && hasButton(GetTrack(NULL, row), b)) {
                g_drag = b;
                g_dragOn = !isOn(GetTrack(NULL, row), b);
            }
        }
    }
    // dragging goes by row only, so a drag that drifts sideways keeps setting the same column
    if (g_drag == NO_BUTTON) {
        return down; // a press on a row's name: nothing to do, but not the panel's either
    }
    if (row >= 0) {
        MediaTrack *tr = GetTrack(NULL, row);
        if (hasButton(tr, g_drag) && isOn(tr, g_drag) != g_dragOn) {
            setOn(tr, g_drag, g_dragOn);
            redraw();
        }
    }
    return true;
}

void finishTracksDrag() { g_drag = NO_BUTTON; }

void drawTrackRows(Canvas &cv, HDC dc, int rowsTop) {
    const Rect p = panelArea();
    int depth = 0; // folder level: a track's I_FOLDERDEPTH applies to the tracks after it
    const int count = CountTracks(NULL);
    for (int i = 0; i < count; i++) {
        MediaTrack *tr = GetTrack(NULL, i);
        const int top = rowsTop + i * ROW_H;
        const int l = p.l + MARGIN + depth * INDENT;
        depth = std::max(0, depth + static_cast<int>(GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH")));
        if (top + ROW_H < p.t || top > p.b) {
            continue;
        }

        // the track's colour as a bar, then its name
        if (const int native = GetTrackColor(tr); native & 0x1000000) {
            int r = 0, g = 0, b = 0;
            ColorFromNative(native & 0xffffff, &r, &g, &b);
            cv.fill(l, top + 4, l + 4, top + ROW_H - 4,
                    static_cast<RGB>((r & 255) << 16 | (g & 255) << 8 | (b & 255)));
        }
        char name[256] = "";
        GetTrackName(tr, name, sizeof(name));
        const int nameR = buttonSpan(GUIDE).first - 6;
        text(dc, name, l + 10, top, nameR, top + ROW_H, g_theme.text, DT_LEFT | DT_END_ELLIPSIS);

        for (const Button b : {GUIDE, MUTE, SOLO}) {
            if (!hasButton(tr, b)) {
                continue;
            }
            const auto [bl, br] = buttonSpan(b);
            const bool on = isOn(tr, b);
            const RGB onFill = b == GUIDE ? g_theme.accent : b == MUTE ? MUTE_ON : SOLO_ON;
            cv.fill(bl, top + 3, br, top + ROW_H - 3, on ? onFill : g_theme.button);
            text(dc, b == GUIDE ? "G" : b == MUTE ? "M" : "S", bl, top + 3, br, top + ROW_H - 3,
                 on ? g_theme.onText : g_theme.dim, DT_CENTER);
        }
    }
}
