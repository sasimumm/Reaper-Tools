#include "lyrics.h"

#include "takesync.h"

static double g_editStart;
static int g_editPitch;

// --- lyrics ------------------------------------------------------------------------------

static int findNote(double start, int pitch) {
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        if (fabs(g_session.ed.notes[i].start - start) < 1e-6 &&
            g_session.ed.notes[i].pitch == pitch) {
            return (int)i;
        }
    }
    return -1;
}

// The note after (dir > 0) or before (dir < 0) note i in time; same-start notes ordered by pitch.
static int neighbourNote(int i, int dir) {
    const Note &c = g_session.ed.notes[i];
    int best = -1;
    for (size_t j = 0; j < g_session.ed.notes.size(); j++) {
        const Note &o = g_session.ed.notes[j];
        const bool after = o.start > c.start || (o.start == c.start && o.pitch > c.pitch);
        const bool before = o.start < c.start || (o.start == c.start && o.pitch < c.pitch);
        if ((int)j == i || (dir > 0 ? !after : !before)) {
            continue;
        }
        if (best < 0) {
            best = (int)j;
            continue;
        }
        const Note &b = g_session.ed.notes[best];
        const bool closer = dir > 0
                                ? (o.start < b.start || (o.start == b.start && o.pitch < b.pitch))
                                : (o.start > b.start || (o.start == b.start && o.pitch > b.pitch));
        if (closer) {
            best = (int)j;
        }
    }
    return best;
}

void placeEdit() {
    const int i = findNote(g_editStart, g_editPitch);
    if (!g_ui.edit || i < 0) {
        return;
    }
    const Note &nt = g_session.ed.notes[i];
    const int x = (int)lround(g_ui.view.x(nt.start)), y = (int)lround(g_ui.view.y(nt.pitch - 0.5));
    const int w = std::max(90, (int)lround(g_ui.view.x(nt.end)) - x);
    RECT r;
    GetWindowRect(g_ui.edit, &r);
    POINT pt = {r.left, r.top};
    ScreenToClient(g_ui.hwnd, &pt);
    if (pt.x != x || pt.y != y + 1 ||
        r.right - r.left != w) { // moving it repaints us, so only when needed
        SetWindowPos(g_ui.edit, NULL, x, y + 1, w, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void beginLyric(int i) {
    if (i < 0 || i >= (int)g_session.ed.notes.size()) {
        return;
    }
    // scroll so the note is visible
    const Rect n = noteArea();
    const Note &nt = g_session.ed.notes[i];
    if (g_ui.view.x(nt.start) < n.l + 20 || g_ui.view.x(nt.start) > n.r - 100) {
        g_ui.view.t0 = nt.start - 60 / g_ui.view.pps - (n.l - g_ui.view.x0) / g_ui.view.pps;
    }
    if (g_ui.view.y(nt.pitch) < n.t + 20 || g_ui.view.y(nt.pitch) > n.b - 40) {
        g_ui.view.pitchTop = nt.pitch + 0.5 * (n.b - n.t) / g_ui.view.rowH;
    }
    for (Note &o : g_session.ed.notes) {
        o.sel = false;
    }
    g_session.ed.notes[i].sel = true;
    g_editStart = nt.start, g_editPitch = nt.pitch;
    if (!g_ui.edit) {
        SWELL_MakeSetCurParms(1, 1, 0, 0, g_ui.hwnd, false, false);
        g_ui.edit = SWELL_MakeEditField(1001, 0, 0, 90, 20, 0);
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
    int i = findNote(g_editStart, g_editPitch);
    if (i < 0) {
        return;
    }
    std::string text = buf;
    while (!text.empty() && isspace((unsigned char)text.back())) {
        text.pop_back();
    }
    while (!text.empty() && isspace((unsigned char)text[0])) {
        text.erase(0, 1);
    }
    const int next = dir ? neighbourNote(i, dir) : -1;
    double nextStart = 0;
    int nextPitch = -1;
    if (next >= 0) {
        nextStart = g_session.ed.notes[next].start, nextPitch = g_session.ed.notes[next].pitch;
    }
    if (commit && text != g_session.ed.notes[i].lyric) {
        g_session.ed.notes[i].lyric = text;
        commitNotes();
    }
    if (nextPitch >= 0) {
        beginLyric(findNote(nextStart, nextPitch));
    }
    redraw();
}

std::string clipboardText() {
    std::string text;
    if (!OpenClipboard(g_ui.hwnd)) {
        return text;
    }
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char *p = (const char *)GlobalLock(h)) {
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

    std::vector<int> order(g_session.ed.notes.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = (int)i;
    }
    std::sort(order.begin(), order.end(), [](int a, int b) {
        const Note &x = g_session.ed.notes[a], &y = g_session.ed.notes[b];
        return x.start < y.start || (x.start == y.start && x.pitch < y.pitch);
    });
    int selected = 0;
    for (const Note &nt : g_session.ed.notes) {
        selected += nt.sel;
    }
    std::vector<int> targets;
    bool started = selected == 0;
    for (int i : order) {
        if (selected > 1 ? g_session.ed.notes[i].sel : (started |= g_session.ed.notes[i].sel)) {
            targets.push_back(i);
        }
    }
    for (size_t k = 0; k < targets.size() && k < syl.size(); k++) {
        g_session.ed.notes[targets[k]].lyric = syl[k];
    }
    commitNotes();
}
