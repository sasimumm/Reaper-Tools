// Typing lyrics into an edit field over a note, moving between notes with tab, and pasting lyrics
// from the clipboard.
#include "notes/lyrics.h"

#include "index.h"
#include "notes/takesync.h"

#include <tuple>

// The note being typed into, by start (seconds), channel and pitch: indices change when notes are
// reloaded. And the take as it was then: if it is reloaded meanwhile (edited elsewhere, another
// take), the typed text is dropped rather than written over what changed.
static double g_editStart;
static int g_editChan, g_editPitch;
static TakeSnapshot g_editSnapshot;

// The edit field: at least this wide and this high, in pixels.
static const int EDIT_MIN_WIDTH = 90, EDIT_HEIGHT = 20;

// --- lyrics ------------------------------------------------------------------------------

// The note starting at `start` (within a microsecond) with this channel and pitch, or -1.
static int findNote(double start, int chan, int pitch) {
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        const Note &n = g_session.ed.notes[i];
        if (fabs(n.start - start) < 1e-6 && n.chan == chan && n.pitch == pitch) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Notes in lyric order: by start, then pitch, then channel.
static bool lyricOrder(const Note &x, const Note &y) {
    return std::tie(x.start, x.pitch, x.chan) < std::tie(y.start, y.pitch, y.chan);
}

// The note after (dir > 0) or before (dir < 0) note i in lyric order.
static int neighbourNote(int i, int dir) {
    const Note &c = g_session.ed.notes[idx(i)];
    int best = -1;
    for (size_t j = 0; j < g_session.ed.notes.size(); j++) {
        const Note &o = g_session.ed.notes[j];
        if (static_cast<int>(j) == i || !(dir > 0 ? lyricOrder(c, o) : lyricOrder(o, c))) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(j);
            continue;
        }
        const Note &b = g_session.ed.notes[idx(best)];
        if (dir > 0 ? lyricOrder(o, b) : lyricOrder(b, o)) {
            best = static_cast<int>(j);
        }
    }
    return best;
}

// Keeps the edit field just below its note's row, as wide as the note.
void placeEdit() {
    const int i = findNote(g_editStart, g_editChan, g_editPitch);
    if (!g_ui.edit || i < 0) {
        return;
    }
    const Note &nt = g_session.ed.notes[idx(i)];
    const int x = static_cast<int>(lround(g_ui.view.x(nt.start)));
    const int y = static_cast<int>(lround(g_ui.view.y(nt.pitch - 0.5)));
    const int w = std::max(EDIT_MIN_WIDTH, static_cast<int>(lround(g_ui.view.x(nt.end))) - x);
    RECT r;
    GetWindowRect(g_ui.edit, &r);
    POINT pt = {r.left, r.top};
    ScreenToClient(g_ui.hwnd, &pt);
    // moving it repaints us, so only when needed
    if (pt.x != x || pt.y != y + 1 || r.right - r.left != w) {
        SetWindowPos(g_ui.edit, NULL, x, y + 1, w, EDIT_HEIGHT, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Starts typing the lyric of note i: scrolls it into view, selects just it and opens the field.
void beginLyric(int i) {
    if (i < 0 || i >= static_cast<int>(g_session.ed.notes.size())) {
        return;
    }

    // scroll so the note is visible: its start 60 px into the note area, its row in the middle
    const Rect n = noteArea();
    const Note &nt = g_session.ed.notes[idx(i)];
    if (g_ui.view.x(nt.start) < n.l + 20 || g_ui.view.x(nt.start) > n.r - 100) {
        g_ui.view.t0 = nt.start - 60 / g_ui.view.pps - (n.l - g_ui.view.x0) / g_ui.view.pps;
    }
    if (g_ui.view.y(nt.pitch) < n.t + 20 || g_ui.view.y(nt.pitch) > n.b - 40) {
        g_ui.view.pitchTop = nt.pitch + 0.5 * (n.b - n.t) / g_ui.view.rowH;
    }

    for (Note &o : g_session.ed.notes) {
        o.sel = false;
    }
    g_session.ed.notes[idx(i)].sel = true;
    g_editStart = nt.start;
    g_editChan = nt.chan;
    g_editPitch = nt.pitch;
    g_editSnapshot = g_session.snapshot;

    if (!g_ui.edit) {
        // SWELL creates controls in the window set as current parent
        SWELL_MakeSetCurParms(1, 1, 0, 0, g_ui.hwnd, false, false);
        g_ui.edit = SWELL_MakeEditField(1001, 0, 0, EDIT_MIN_WIDTH, EDIT_HEIGHT, 0);
        SWELL_MakeSetCurParms(1, 1, 0, 0, NULL, false, false);
    }
    if (!g_ui.edit) {
        return;
    }
    SetWindowText(g_ui.edit, nt.lyric.c_str());
    placeEdit();
    ShowWindow(g_ui.edit, SW_SHOW);
    SetFocus(g_ui.edit);
    SendMessage(g_ui.edit, EM_SETSEL, 0, -1);
    redraw();
}

// Strips leading and trailing whitespace.
static std::string trimmed(std::string text) {
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    while (!text.empty() && isspace(static_cast<unsigned char>(text[0]))) {
        text.erase(0, 1);
    }
    return text;
}

// commit: store the typed text; dir: then edit the next (1) / previous (-1) note, or stop (0)
void endLyric(bool commit, int dir) {
    if (!g_ui.edit) {
        return;
    }
    char buf[1024] = "";
    GetWindowText(g_ui.edit, buf, sizeof(buf));
    DestroyWindow(g_ui.edit);
    g_ui.edit = NULL;
    SetFocus(g_ui.hwnd);

    int i = findNote(g_editStart, g_editChan, g_editPitch);
    if (i < 0 || g_session.snapshot != g_editSnapshot) {
        redraw();
        return;
    }
    const std::string text = trimmed(buf);

    // the next note by start and pitch, as committing reloads the notes
    const int next = dir ? neighbourNote(i, dir) : -1;
    double nextStart = 0;
    int nextChan = 0, nextPitch = -1;
    if (next >= 0) {
        nextStart = g_session.ed.notes[idx(next)].start;
        nextChan = g_session.ed.notes[idx(next)].chan;
        nextPitch = g_session.ed.notes[idx(next)].pitch;
    }

    if (commit && text != g_session.ed.notes[idx(i)].lyric) {
        g_session.ed.notes[idx(i)].lyric = text;
        commitNotes();
    }
    if (nextPitch >= 0) {
        beginLyric(findNote(nextStart, nextChan, nextPitch));
    }
    redraw();
}

// The clipboard's text, "" if there is none.
std::string clipboardText() {
    std::string text;
    if (!OpenClipboard(g_ui.hwnd)) {
        return text;
    }
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char *p = static_cast<const char *>(GlobalLock(h))) {
            text = p;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return text;
}

// paste lyrics: one syllable per note, to the selected notes (several selected) or from the
// selected note onwards (one selected, or none: from the first note).
void pasteLyrics(const std::string &text) {
    const std::vector<std::string> syl = splitSyllables(text);
    if (syl.empty()) {
        return;
    }

    // note indices in lyric order
    std::vector<int> order(g_session.ed.notes.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return lyricOrder(g_session.ed.notes[idx(a)], g_session.ed.notes[idx(b)]);
    });

    int selected = 0;
    for (const Note &nt : g_session.ed.notes) {
        selected += nt.sel;
    }
    std::vector<int> targets;
    bool started = selected == 0; // with one selected, from it on
    for (int i : order) {
        const bool sel = g_session.ed.notes[idx(i)].sel;
        started = started || sel;
        if (selected > 1 ? sel : started) {
            targets.push_back(i);
        }
    }

    for (size_t k = 0; k < targets.size() && k < syl.size(); k++) {
        g_session.ed.notes[idx(targets[k])].lyric = syl[k];
    }
    commitNotes();
}

void finishLyric() {
    if (!g_ui.edit) {
        return;
    }
    // committing without the item or its notes take would create a new notes item (or touch a
    // deleted one): then the typed text is dropped with the field
    const bool keep =
        g_session.audio && ValidatePtr2(NULL, g_session.audio, "MediaItem*") && midiTake();
    endLyric(keep, 0);
}
