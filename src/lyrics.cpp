#include "ui.h"

static double g_editStart;
static int g_editPitch;

// --- lyrics ------------------------------------------------------------------------------

static int findNote(double start, int pitch) {
    for (size_t i = 0; i < g_ed.notes.size(); i++) {
        if (fabs(g_ed.notes[i].start - start) < 1e-6 && g_ed.notes[i].pitch == pitch) {
            return (int)i;
        }
    }
    return -1;
}

// The note after (dir > 0) or before (dir < 0) note i in time; same-start notes ordered by pitch.
static int neighbourNote(int i, int dir) {
    const Note &c = g_ed.notes[i];
    int best = -1;
    for (size_t j = 0; j < g_ed.notes.size(); j++) {
        const Note &o = g_ed.notes[j];
        const bool after = o.start > c.start || (o.start == c.start && o.pitch > c.pitch);
        const bool before = o.start < c.start || (o.start == c.start && o.pitch < c.pitch);
        if ((int)j == i || (dir > 0 ? !after : !before)) {
            continue;
        }
        if (best < 0) {
            best = (int)j;
            continue;
        }
        const Note &b = g_ed.notes[best];
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
    if (!g_edit || i < 0) {
        return;
    }
    const Note &nt = g_ed.notes[i];
    const int x = (int)lround(g_view.x(nt.start)), y = (int)lround(g_view.y(nt.pitch - 0.5));
    const int w = std::max(90, (int)lround(g_view.x(nt.end)) - x);
    RECT r;
    GetWindowRect(g_edit, &r);
    POINT pt = {r.left, r.top};
    ScreenToClient(g_hwnd, &pt);
    if (pt.x != x || pt.y != y + 1 ||
        r.right - r.left != w) { // moving it repaints us, so only when needed
        SetWindowPos(g_edit, NULL, x, y + 1, w, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void beginLyric(int i) {
    if (i < 0 || i >= (int)g_ed.notes.size()) {
        return;
    }
    // scroll so the note is visible
    const Rect n = noteArea();
    const Note &nt = g_ed.notes[i];
    if (g_view.x(nt.start) < n.l + 20 || g_view.x(nt.start) > n.r - 100) {
        g_view.t0 = nt.start - 60 / g_view.pps - (n.l - g_view.x0) / g_view.pps;
    }
    if (g_view.y(nt.pitch) < n.t + 20 || g_view.y(nt.pitch) > n.b - 40) {
        g_view.pitchTop = nt.pitch + 0.5 * (n.b - n.t) / g_view.rowH;
    }
    for (Note &o : g_ed.notes) {
        o.sel = false;
    }
    g_ed.notes[i].sel = true;
    g_editStart = nt.start, g_editPitch = nt.pitch;
    if (!g_edit) {
        SWELL_MakeSetCurParms(1, 1, 0, 0, g_hwnd, false, false);
        g_edit = SWELL_MakeEditField(1001, 0, 0, 90, 20, 0);
        SWELL_MakeSetCurParms(1, 1, 0, 0, NULL, false, false);
    }
    if (!g_edit) {
        return;
    }
    SetWindowText(g_edit, nt.lyric.c_str());
    placeEdit();
    ShowWindow(g_edit, SW_SHOW);
    SetFocus(g_edit);
    SendMessage(g_edit, EM_SETSEL, 0, -1);
    redraw();
}

// commit: store the typed text; dir: then edit the next (1) / previous (-1) note, or stop (0)
void endLyric(bool commit, int dir) {
    if (!g_edit) {
        return;
    }
    char buf[1024] = "";
    GetWindowText(g_edit, buf, sizeof(buf));
    DestroyWindow(g_edit);
    g_edit = NULL;
    SetFocus(g_hwnd);
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
        nextStart = g_ed.notes[next].start, nextPitch = g_ed.notes[next].pitch;
    }
    if (commit && text != g_ed.notes[i].lyric) {
        g_ed.notes[i].lyric = text;
        commitNotes();
    }
    if (nextPitch >= 0) {
        beginLyric(findNote(nextStart, nextPitch));
    }
    redraw();
}

std::string clipboardText() {
    std::string text;
    if (!OpenClipboard(g_hwnd)) {
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

    std::vector<int> order(g_ed.notes.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = (int)i;
    }
    std::sort(order.begin(), order.end(), [](int a, int b) {
        const Note &x = g_ed.notes[a], &y = g_ed.notes[b];
        return x.start < y.start || (x.start == y.start && x.pitch < y.pitch);
    });
    int selected = 0;
    for (const Note &nt : g_ed.notes) {
        selected += nt.sel;
    }
    std::vector<int> targets;
    bool started = selected == 0;
    for (int i : order) {
        if (selected > 1 ? g_ed.notes[i].sel : (started |= g_ed.notes[i].sel)) {
            targets.push_back(i);
        }
    }
    for (size_t k = 0; k < targets.size() && k < syl.size(); k++) {
        g_ed.notes[targets[k]].lyric = syl[k];
    }
    commitNotes();
}
