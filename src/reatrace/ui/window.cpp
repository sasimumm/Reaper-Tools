// The ReaTrace window: its lifecycle and docking, the message handler (mouse, wheel, pinch),
// keyboard shortcuts, view navigation, and the timer that follows changes in REAPER.
#include "ui/window.h"

#include "index.h"
#include "state.h"

#include "analysis.h"
#include "notes/commands.h"
#include "notes/lyrics.h"
#include "notes/takesync.h"
#include "playback/isolate.h"
#include "playback/preview.h"
#include "settings.h"
#include "shared/guard.h"
#include "ui/paint.h"
#include "ui/panel.h"
#include "ui/toolbar.h"

// Zoom per pinch event: SWELL clamps each event's factor to 0.5..2, so only its direction is used.
static const double PINCH_STEP = 1.03;
static const double WHEEL_ZOOM = 1.05; // zoom per wheel notch (cmd/alt+scroll)
static const double MAX_PPS = 3000;    // zoom limit, points per second
// row height limits when zooming pitch, points per semitone
static const double MIN_ROW_H = 3, MAX_ROW_H = 60;
// the pitch range fitItem() shows: E2..C6, the vocal range
static const int FIT_LOW = 40, FIT_HIGH = 84;
// seconds the item must stay unchanged after a move or trim before it is analysed again
static const double REANALYSE_DELAY = 0.3;

static bool g_docked;     // in REAPER's docker rather than a floating window (remembered)
static bool g_recreating; // destroying the window only to recreate it: keep the state

// What the mouse button went down on, until it goes up.
enum Drag { DRAG_NONE, DRAG_NOTES, DRAG_RULER, DRAG_RULER_SELECT, DRAG_PANEL, DRAG_OVERVIEW };
static Drag g_drag;
static double g_selectAnchor; // DRAG_RULER_SELECT: where the time selection started, seconds
static int g_pressedSuggestion = -1; // DRAG_NOTES: the suggestion pressed, accepted on a click

// change detection for the timer
static int g_lastPlayState, g_lastProjChange;
static double g_lastPlayPos, g_lastCursor;

Layout layout() {
    RECT r = {0, 0, 0, 0};
    if (g_ui.hwnd) {
        GetClientRect(g_ui.hwnd, &r);
    }
    return computeLayout(static_cast<int>(r.right), static_cast<int>(r.bottom), g_ui.panel,
                         g_ui.foldedLanes);
}

Rect clientRect() { return layout().client; }
bool panelShown() { return layout().panelShown; }
Rect noteArea() { return layout().notes; }
Rect rulerArea() { return layout().ruler; }
Rect chordsArea() { return layout().chords; }
Rect lyricsArea() { return layout().lyrics; }
Rect overviewArea() { return layout().overview; }
Rect panelArea() { return layout().panel; }

void syncViewOrigin() {
    Rect n = noteArea();
    g_ui.view.x0 = n.l;
    g_ui.view.y0 = n.t;
}

void redraw() {
    if (g_ui.hwnd) {
        InvalidateRect(g_ui.hwnd, NULL, FALSE);
    }
}

// --- navigation --------------------------------------------------------------------

// The whole item with a 1% margin on each side, and the vocal range.
static void fitItem() {
    syncViewOrigin();
    const Rect n = noteArea();
    g_ui.view.pps = std::max(5.0, (n.r - n.l) / std::max(0.1, g_session.audioLen * 1.02));
    g_ui.view.t0 = g_session.audioPos - 0.01 * g_session.audioLen;
    g_ui.view.rowH = std::max(MIN_ROW_H, static_cast<double>(n.b - n.t) / (FIT_HIGH - FIT_LOW + 1));
    g_ui.view.pitchTop = FIT_HIGH + 0.5;
    redraw();
}

// The arrange view's time range, with fitItem's pitch range; the whole item when the arrange view
// doesn't show any of it.
static void matchArrange() {
    double a = 0, b = 0;
    GetSet_ArrangeView2(NULL, false, 0, 0, &a, &b);
    fitItem();
    if (b <= a || b <= g_session.audioPos || a >= g_session.audioPos + g_session.audioLen) {
        return;
    }
    const Rect n = noteArea();
    g_ui.view.pps = (n.r - n.l) / (b - a);
    g_ui.view.t0 = a;
    redraw();
}

// The selected notes, with some room around them.
static void zoomToSelection() {
    double a = 1e30, b = -1e30;
    int lo = 128, hi = -1;
    for (const Note &nt : g_session.ed.notes) {
        if (nt.sel) {
            a = std::min(a, nt.start);
            b = std::max(b, nt.end);
            lo = std::min(lo, nt.pitch);
            hi = std::max(hi, nt.pitch);
        }
    }
    if (hi < 0) {
        return;
    }
    const Rect n = noteArea();
    const double len = std::max(0.2, b - a);
    g_ui.view.pps = std::min(MAX_PPS, (n.r - n.l) / (len * 1.2));
    g_ui.view.t0 = a - len * 0.1;
    // three semitones above and below
    const double rowH = static_cast<double>(n.b - n.t) / (hi - lo + 7);
    g_ui.view.rowH = std::min(MAX_ROW_H, std::max(MIN_ROW_H, rowH));
    g_ui.view.pitchTop = hi + 3.5;
    redraw();
}

// Scrolls time t (seconds) to the middle of the notes area.
static void centreOn(double t) {
    const Rect n = noteArea();
    g_ui.view.t0 = t - 0.5 * (n.r - n.l) / g_ui.view.pps - (n.l - g_ui.view.x0) / g_ui.view.pps;
    redraw();
}

// Zooms time by `factor`, keeping the time at client x where it is.
static void zoomTime(double factor, int x) {
    const double t = g_ui.view.t(x);
    g_ui.view.pps = std::min(MAX_PPS, std::max(2.0, g_ui.view.pps * factor));
    g_ui.view.t0 = t - (x - g_ui.view.x0) / g_ui.view.pps;
    redraw();
}

// Zooms pitch by `factor`, keeping the pitch at client y where it is.
static void zoomPitch(double factor, int y) {
    const double p = g_ui.view.p(y);
    g_ui.view.rowH = std::min(MAX_ROW_H, std::max(MIN_ROW_H, g_ui.view.rowH * factor));
    g_ui.view.pitchTop = p + (y - g_ui.view.y0) / g_ui.view.rowH;
    redraw();
}

// Scrolls up by `semitones`, keeping the view within MIDI pitches 0..127.
static void scrollPitch(double semitones) {
    const double lowest = (noteArea().b - noteArea().t) / g_ui.view.rowH - 0.5;
    g_ui.view.pitchTop = std::min(128.0, std::max(g_ui.view.pitchTop + semitones, lowest));
    redraw();
}

// Linked to the arrange view: whichever side changed since the last tick moves the other one.
// Starts by following the arrange view.
static double g_arrA = -1, g_arrB = -1, g_ourA, g_ourB; // ranges as of the last sync

static void syncArrange() {
    if (!g_settings.linkArrange) {
        g_arrA = g_arrB = -1;
        return;
    }
    syncViewOrigin();
    const Rect n = noteArea();
    double a = 0, b = 0;
    GetSet_ArrangeView2(NULL, false, 0, 0, &a, &b);
    if (b <= a) {
        return;
    }
    if (a != g_arrA || b != g_arrB) {
        g_ui.view.pps = (n.r - n.l) / (b - a);
        g_ui.view.t0 = a;
        redraw();
    } else if (g_ui.view.t(n.l) != g_ourA || g_ui.view.t(n.r) != g_ourB) {
        a = g_ui.view.t(n.l);
        b = g_ui.view.t(n.r);
        GetSet_ArrangeView2(NULL, true, 0, 0, &a, &b);
        GetSet_ArrangeView2(NULL, false, 0, 0, &a, &b); // read back what REAPER made of it
    }
    g_arrA = a;
    g_arrB = b;
    g_ourA = g_ui.view.t(n.l);
    g_ourB = g_ui.view.t(n.r);
}

// Pages the view along with the play cursor (see followPage()).
static void followPlayback() {
    static double s_prev = -1; // play position last tick, -1 when stopped
    const double now = GetPlayPosition2();
    if (!(GetPlayState() & 1)) {
        s_prev = -1;
        return;
    }
    if (g_settings.followPlay && g_drag == DRAG_NONE && s_prev >= 0) {
        syncViewOrigin();
        const Rect n = noteArea();
        const double t0 = g_ui.view.t(n.l);
        const double page = followPage(t0, (n.r - n.l) / g_ui.view.pps, s_prev, now);
        if (page != t0) {
            g_ui.view.t0 += page - t0;
            redraw();
        }
    }
    s_prev = now;
}

void selectVoice(int v) {
    if (v < 0 || v >= WINDOW_VOICES) {
        return;
    }
    g_ui.hiddenVoices &= ~(1u << v); // the edited voice is shown
    if (v == g_session.voice) {
        redraw();
        return;
    }
    if (g_ui.edit) {
        endLyric(true, 0);
    }
    g_session.ed.cancel();
    g_session.voice = g_ui.voicePick = v;
    g_session.midi = VoiceItem(g_session.audio, v, false); // created with the first note
    loadNotes();
    loadOtherVoices(true);
    redraw();
}

// --- input -------------------------------------------------------------------------

int mods() {
    // the high bit: held now
    return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) |
           ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_CMD : 0) |
           ((GetAsyncKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0);
}

// Changes the transform's resolution (Hz) within the slider's range.
static void adjustGamma(double delta) {
    g_settings.np.gamma = std::max(3.0, std::min(40.0, g_settings.np.gamma + delta));
    requestAnalysis(false);
}

// Punctuation arrives as a character code without FVIRTKEY, and some collide with VK codes
// ('.' == VK_DELETE), so keys are encoded as key | CHARKEY for characters.
static const int CHARKEY = 0x10000;
static int keyCode(int key, int flags) { return (flags & FVIRTKEY) ? key : key | CHARKEY; }

// A shortcut (keyCode() form); true if handled.
static bool onKey(int key) {
    const int m = mods();
    if (key == VK_DELETE || key == VK_BACK) {
        if (g_session.ed.deleteSelected()) {
            commitNotes();
        }
    } else if (key == ('?' | CHARKEY) || (key == ('/' | CHARKEY) && (m & MOD_SHIFT))) {
        g_ui.help = !g_ui.help;
    } else if (key == VK_ESCAPE && g_ui.help) {
        g_ui.help = false;
    } else if (key == VK_ESCAPE) {
        if (g_session.ed.active()) {
            g_session.ed.cancel();
            g_drag = DRAG_NONE;
            ReleaseCapture();
        } else if (g_session.ed.selectAll(false)) {
            commitNotes();
        }
    } else if (key == 'A' && (m & MOD_CMD)) {
        if (g_session.ed.selectAll(true)) {
            commitNotes();
        }
    } else if (key == VK_RETURN) {
        int i = firstSelected();
        if (i < 0) { // nothing selected: the note under the mouse
            g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY, &i);
        }
        beginLyric(i);
    } else if (key == 'H' && !m) {
        g_settings.ghosts = !g_settings.ghosts;
    } else if (key == 'S' && !m) {
        g_ui.panel = !g_ui.panel;
    } else if (key == 'G' && !m) {
        g_settings.snapMode = (g_settings.snapMode + 1) % 3;
        setupEditorSnap();
    } else if (key == 'P' && !m) {
        g_settings.showContour = !g_settings.showContour;
    } else if (key == 'X' && !m) {
        g_settings.explain = !g_settings.explain;
    } else if (key >= '1' && key < '1' + WINDOW_VOICES && !m) {
        selectVoice(key - '1');
    } else if ((key == VK_LEFT || key == VK_RIGHT) && !(m & ~MOD_SHIFT)) {
        nudgeSelected(key == VK_LEFT ? -1 : 1, 0);
    } else if ((key == VK_UP || key == VK_DOWN) && !(m & ~MOD_SHIFT)) {
        nudgeSelected(0, (key == VK_UP ? 1 : -1) * (m & MOD_SHIFT ? 12 : 1));
    } else if (key == 'Q' && !m) {
        quantiseSelected();
    } else if (key == 'D' && !m) {
        g_ui.dockPending = true;
    } else if (key == 'L' && !m) {
        g_settings.linkArrange = !g_settings.linkArrange;
    } else if (key == 'I' && !m) {
        toggleIsolate(false);
    } else if (key == 'U' && !m) {
        toggleIsolate(true);
    } else if (key == 'I' && m == MOD_SHIFT) {
        g_settings.isolateAll = !g_settings.isolateAll;
    } else if (key == 'T' && !m) {
        g_settings.followPlay = !g_settings.followPlay;
    } else if (key == 'N' && !m) {
        g_settings.suggest = !g_settings.suggest;
    } else if (key == 'A' && !m) {
        acceptVisibleSuggestions();
    } else if (key == 'V' && (m & MOD_CMD)) {
        paste();
    } else if (key == 'C' && (m & MOD_CMD)) {
        copyNotes(false);
    } else if (key == 'X' && (m & MOD_CMD)) {
        copyNotes(true);
    } else if (key == 'D' && (m & MOD_CMD)) {
        if (g_session.ed.duplicateSelected()) {
            commitNotes();
        }
    } else if (key == 'K' && !m) {
        splitNotes();
    } else if (key == 'R' && !m) {
        loopPlay();
    } else if (key == 'B' && !m) {
        swapListen();
    } else if (key == 'J' && !m) {
        if (g_session.ed.joinSelected()) {
            commitNotes();
        }
    } else if (key == 'F' && !m) {
        fitItem();
    } else if (key == 'Z' && !m) {
        zoomToSelection();
    } else if (key == ('[' | CHARKEY)) {
        g_settings.rp.brightness = std::max(-1.0, g_settings.rp.brightness - 0.05);
        updateValues();
    } else if (key == (']' | CHARKEY)) {
        g_settings.rp.brightness = std::min(1.0, g_settings.rp.brightness + 0.05);
        updateValues();
    } else if (key == ('-' | CHARKEY)) {
        g_settings.rp.contrast = std::max(0.1, g_settings.rp.contrast / 1.1);
        updateValues();
    } else if (key == ('=' | CHARKEY)) {
        g_settings.rp.contrast = std::min(3.0, g_settings.rp.contrast * 1.1);
        updateValues();
    } else if (key == (',' | CHARKEY)) {
        adjustGamma(-1);
    } else if (key == ('.' | CHARKEY)) {
        adjustGamma(1);
    } else {
        return false;
    }
    redraw();
    return true;
}

// Whether key (keyCode() form) with accelerator flags (FCONTROL, ...) is one of our shortcuts;
// the rest stay REAPER's.
static bool isOurKey(int key, int flags) {
    const bool cmd = (flags & FCONTROL) != 0;
    switch (key) {
    case VK_DELETE:
    case VK_BACK:
    case VK_ESCAPE:
    case VK_RETURN:
    case '[' | CHARKEY:
    case ']' | CHARKEY:
    case '-' | CHARKEY:
    case '=' | CHARKEY:
    case ',' | CHARKEY:
    case '.' | CHARKEY:
    case '?' | CHARKEY: return true;
    case '/' | CHARKEY: return (flags & FSHIFT) != 0;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN: // with notes selected; otherwise REAPER's (move the edit cursor, ...)
        return !cmd && !(flags & FALT) && firstSelected() >= 0;
    case 'A': return true;
    case 'I': return !cmd && !(flags & FALT);
    case 'V':
    case 'C': return cmd;
    case 'X':
    case 'D': return cmd || !(flags & (FALT | FSHIFT));
    case '1':
    case '2':
    case '3':
    case '4': return !cmd && !(flags & (FALT | FSHIFT));
    case 'S':
    case 'G':
    case 'F':
    case 'Z':
    case 'H':
    case 'P':
    case 'N':
    case 'Q':
    case 'L':
    case 'T':
    case 'K':
    case 'J':
    case 'R':
    case 'B': return !cmd && !(flags & (FALT | FSHIFT));
    }
    return false;
}

// Returns 0: not for us, 1: handled, -1: to the focused control, -666: to REAPER's main window.
int Window_TranslateAccel(MSG *msg) {
    if (!g_ui.hwnd || (msg->hwnd != g_ui.hwnd && !IsChild(g_ui.hwnd, msg->hwnd))) {
        return 0;
    }
    if (g_ui.edit && msg->hwnd != g_ui.hwnd) {
        // typing a lyric: Tab/Space next, shift+Tab previous, Enter done, Esc cancel; the rest goes
        // to the field
        if (msg->message == WM_KEYDOWN) {
            const int key = static_cast<int>(msg->wParam), flags = static_cast<int>(msg->lParam);
            if (key == VK_TAB) {
                endLyric(true, (flags & FSHIFT) ? -1 : 1);
                return 1;
            }
            if (key == VK_SPACE && (flags & FVIRTKEY)) {
                endLyric(true, 1);
                return 1;
            }
            if (key == '-' && !(flags & FVIRTKEY)) { // hyphen: this syllable joins the next one
                char buf[1024] = "";
                GetWindowText(g_ui.edit, buf, sizeof(buf) - 1);
                SetWindowText(g_ui.edit, (std::string(buf) + "-").c_str());
                endLyric(true, 1);
                return 1;
            }
            if (key == VK_RETURN) {
                endLyric(true, 0);
                return 1;
            }
            if (key == VK_ESCAPE) {
                endLyric(false, 0);
                return 1;
            }
        }
        return -1;
    }

    // Our keys are handled here: delivered to the window, SWELL would turn Enter/Esc into dialog
    // commands.
    const int flags = static_cast<int>(msg->lParam);
    const int key = keyCode(static_cast<int>(msg->wParam), flags);
    if (msg->message == WM_KEYDOWN && isOurKey(key, flags)) {
        onKey(key);
        return 1;
    }
    if (msg->message == WM_KEYUP && isOurKey(key, flags)) {
        return 1;
    }
    return -666; // everything else (space = play/stop, ...) uses REAPER's main shortcuts
}

static INT_PTR handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: return HTCLIENT; // otherwise SWELL turns clicks into WM_NCLBUTTONDOWN etc.
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: redraw(); return 0;
    case WM_KEYDOWN:
        return onKey(keyCode(static_cast<int>(wParam), static_cast<int>(lParam))) ? 0 : 1;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        syncViewOrigin();
        if (g_ui.edit) {
            endLyric(true, 0); // clicking elsewhere finishes the lyric
        }
        SetFocus(hwnd);
        if (x < KB_W && y >= noteArea().t && y < noteArea().b) {
            previewPitch(g_ui.view.pitchAt(y));
            return 0;
        }
        if (layout().toolbar.in(x, y)) {
            toolbarClick(x, y, mods(), layout());
            redraw();
            return 0;
        }
        if (const unsigned lane = laneLabelAt(x, y, layout())) {
            g_ui.foldedLanes ^= lane;
            redraw();
            return 0;
        }
        if (panelShown() && panelArea().in(x, y)) {
            g_drag = DRAG_PANEL;
            panelMouse(x, y, true);
        } else if (rulerArea().in(x, y) && (mods() & MOD_SHIFT)) {
            // shift+drag in the ruler: time selection, snapped like notes
            g_drag = DRAG_RULER_SELECT;
            g_selectAnchor =
                (mods() & MOD_ALT) ? g_ui.view.t(x) : g_session.ed.snap(g_ui.view.t(x));
            setSelection(g_selectAnchor, g_selectAnchor);
        } else if (rulerArea().in(x, y)) {
            g_drag = DRAG_RULER;
            SetEditCurPos(g_ui.view.t(x), false, true);
        } else if (overviewArea().in(x, y) && !(g_ui.foldedLanes & LANE_OVERVIEW)) {
            g_drag = DRAG_OVERVIEW;
            centreOn(overviewTime(x));
        } else if (lyricsArea().in(x, y) && !(g_ui.foldedLanes & LANE_LYRICS)) {
            beginLyric(lyricAt(x));
            return 0;
        } else if (noteArea().in(x, y)) {
            // a plain click on a suggested note, not on a note, makes it a note; a drag draws
            const bool plainMiss =
                g_session.ed.hitTest(g_ui.view, x, y, NULL) == NoteEditor::NONE &&
                !(mods() & (MOD_SHIFT | MOD_CMD));
            g_pressedSuggestion = plainMiss ? suggestionAt(x, y) : -1;

            g_drag = DRAG_NOTES;
            g_session.ed.mouseDown(g_ui.view, x, y, mods());
            int hit = -1;
            if (g_session.ed.hitTest(g_ui.view, x, y, &hit) != NoteEditor::NONE) {
                previewPitch(g_session.ed.notes[idx(hit)].pitch);
            }
            previewDragStarted(hit >= 0 ? g_session.ed.notes[idx(hit)].pitch : -1);
        } else {
            return 0;
        }
        SetCapture(hwnd);
        redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        g_ui.mouseX = x;
        g_ui.mouseY = y;
        if (g_drag == DRAG_NOTES) {
            g_session.ed.mouseMove(g_ui.view, x, y, mods());
            previewDragged();
        } else if (g_drag == DRAG_RULER) {
            SetEditCurPos(g_ui.view.t(x), false, true);
        } else if (g_drag == DRAG_RULER_SELECT) {
            const double t =
                (mods() & MOD_ALT) ? g_ui.view.t(x) : g_session.ed.snap(g_ui.view.t(x));
            setSelection(std::min(t, g_selectAnchor), std::max(t, g_selectAnchor));
        } else if (g_drag == DRAG_PANEL) {
            panelMouse(x, y, false);
        } else if (g_drag == DRAG_OVERVIEW) {
            centreOn(overviewTime(x));
        }
        redraw();
        return 0;
    }
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        const Drag d = g_drag;
        g_drag = DRAG_NONE;
        ReleaseCapture();

        if (d == DRAG_NOTES && g_session.ed.drawing() < 0 && g_pressedSuggestion >= 0) {
            g_session.ed.cancel();
            acceptSuggestion(g_pressedSuggestion);
            g_pressedSuggestion = -1;
            redraw();
            return 0;
        }
        g_pressedSuggestion = -1;
        if (d == DRAG_NOTES && g_session.ed.mouseUp(g_ui.view, x, y, mods())) {
            commitNotes();
        }
        if (d == DRAG_PANEL) {
            finishPanelDrag();
        }
        redraw();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // double-click a note to type its lyric, a slider to reset it
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (panelShown() && panelArea().in(x, y)) {
            g_drag = DRAG_PANEL;
            panelDoubleClick(y); // mouse up recomputes the analysis if needed
            return 0;
        }
        int hit = -1;
        if (noteArea().in(x, y) &&
            g_session.ed.hitTest(g_ui.view, x, y, &hit) != NoteEditor::NONE) {
            beginLyric(hit);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (noteArea().in(x, y) && !g_session.ed.active() &&
            g_session.ed.deleteAt(g_ui.view, x, y)) {
            commitNotes();
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // trackpad friendly: scroll pans, cmd+scroll zooms time, alt+scroll zooms pitch,
        // shift+scroll pans time
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        syncViewOrigin();
        const double steps = static_cast<short> HIWORD(wParam) / 120.0; // notches
        const int m = mods();
        if (panelShown() && panelArea().in(pt.x, pt.y)) {
            if (msg == WM_MOUSEWHEEL) {
                scrollPanel(static_cast<int>(lround(-steps * 30))); // 30 points per notch
            }
            return 0;
        }
        if (msg == WM_MOUSEHWHEEL || ((m & MOD_SHIFT) && !(m & (MOD_CMD | MOD_ALT)))) {
            g_ui.view.t0 -= steps * 40 / g_ui.view.pps; // 40 points per notch
        } else if (m & MOD_CMD) {
            zoomTime(pow(WHEEL_ZOOM, steps), pt.x);
        } else if (m & MOD_ALT) {
            zoomPitch(pow(WHEEL_ZOOM, steps), pt.y);
        } else {
            scrollPitch(steps * 2);
        }
        redraw();
        return 0;
    }
    case WM_GESTURE: {
        // pinch zooms time (alt: pitch); SWELL reports the scale factor *1024 at GF_END
        GESTUREINFO *gi = reinterpret_cast<GESTUREINFO *>(lParam);
        if (gi && gi->dwID == GID_ZOOM && (gi->dwFlags & GF_END) && gi->ullArguments) {
            POINT pt = {gi->ptsLocation.x, gi->ptsLocation.y};
            ScreenToClient(hwnd, &pt);
            syncViewOrigin();
            double f = 1;
            if (gi->ullArguments > 1024) {
                f = PINCH_STEP;
            } else if (gi->ullArguments < 1024) {
                f = 1 / PINCH_STEP;
            }
            if (mods() & MOD_ALT) {
                zoomPitch(f, pt.y);
            } else {
                zoomTime(f, pt.x);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        finishLyric();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_docked) {
            DockWindowRemove(hwnd);
        }
        saveItemSettings();
        stopPreviews(false);
        g_ui.edit = NULL; // destroyed with the window
        if (!g_recreating) {
            setIsolate(false); // it follows this window's notes: don't leave it filtering
            cancelJob();
            clearCache();
        }
        releasePaintCache();
        g_ui.hwnd = NULL;
        g_drag = DRAG_NONE;
        return 0;
    }
    return 0;
}

static INT_PTR WINAPI dlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return guarded("window", static_cast<INT_PTR>(0),
                   [&] { return handleMessage(hwnd, msg, wParam, lParam); });
}

// --- lifecycle ---------------------------------------------------------------------

// SWELL's CreateDialog "resource" for a resizable top-level window without a dialog resource.
static const INT_PTR SWELL_RESIZABLE_WINDOW = 0x400001;

// Floating: a resizable top-level window. Docked: a child window handed to REAPER's docker
// (SWELL makes a child view when there is no dialog resource and no SWELL_RESIZABLE_WINDOW).
static void createWindow() {
    if (g_docked) {
        g_ui.hwnd = CreateDialog(NULL, NULL, GetMainHwnd(), dlgProc);
        if (!g_ui.hwnd) {
            return;
        }
        DockWindowAddEx(g_ui.hwnd, "ReaTrace", "reatrace_editor", true);
        DockWindowActivate(g_ui.hwnd);
    } else {
        g_ui.hwnd = CreateDialog(NULL, reinterpret_cast<const char *>(SWELL_RESIZABLE_WINDOW),
                                 GetMainHwnd(), dlgProc);
        if (!g_ui.hwnd) {
            return;
        }
        SetWindowPos(g_ui.hwnd, NULL, 80, 80, 1300, 760, SWP_NOZORDER);
    }

    MediaItem_Take *take = GetActiveTake(g_session.audio);
    const char *name = take ? GetTakeName(take) : NULL;
    SetWindowText(g_ui.hwnd, (std::string("ReaTrace - ") + (name ? name : "")).c_str());
    ShowWindow(g_ui.hwnd, SW_SHOW);
    matchArrange();
}

// Recreates the window docked or floating, keeping the view; remembered for next time.
static void toggleDock() {
    finishLyric(); // the edit field goes with the window
    const ViewMap view = g_ui.view;
    g_recreating = true;
    if (g_ui.hwnd) {
        DestroyWindow(g_ui.hwnd);
    }
    g_recreating = false;
    g_docked = !g_docked;
    SetExtState(EXT_SECTION, "docked", g_docked ? "1" : "0", true);
    createWindow();
    g_ui.view = view;
    redraw();
}

void Window_Open(MediaItem *audio, int voice) {
    voice = std::max(0, std::min(WINDOW_VOICES - 1, voice));
    if (g_ui.hwnd && audio == g_session.audio) {
        selectVoice(voice);
        ShowWindow(g_ui.hwnd, SW_SHOW);
        SetForegroundWindow(g_ui.hwnd);
        return;
    }

    Window_Close();
    clearCache();
    g_session = Session();
    g_session.audio = audio;
    g_session.voice = g_ui.voicePick = voice;
    g_session.midi = VoiceItem(audio, voice, false);
    g_session.audioPos = GetMediaItemInfo_Value(audio, "D_POSITION");
    g_session.audioLen = GetMediaItemInfo_Value(audio, "D_LENGTH");
    MediaItem_Take *take = GetActiveTake(audio);
    g_session.audioOffs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    g_session.audioRate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    g_session.audioIdentity = audioIdentity(take);

    clearAnalysis();
    const bool hadSettings = loadItemSettings();
    updateValues();
    requestAnalysis(!hadSettings);
    setupEditorSnap();
    loadNotes();
    loadOtherVoices(true);
    loadReference();

    const char *d = globalState("docked");
    g_docked = d && *d == '1';
    createWindow();
}

void Window_Close() {
    if (g_ui.hwnd) {
        finishLyric();
        DestroyWindow(g_ui.hwnd);
    }
    // also without a window: Window_Open starts the analysis before creating it, which can fail
    cancelJob();
}

void Window_Timer() {
    stopPreviews(true);
    if (!g_ui.hwnd) {
        return;
    }
    if (g_ui.dockPending) {
        g_ui.dockPending = false;
        toggleDock();
        if (!g_ui.hwnd) {
            return;
        }
    }
    pollJob();
    syncArrange();
    followPlayback();
    if (g_drag == DRAG_NONE) {
        saveItemSettings();
    }
    if (!ValidatePtr2(NULL, g_session.audio, "MediaItem*")) {
        Window_Close();
        return;
    }

    // the item may have been moved, trimmed, slipped or sped up: re-analyze
    MediaItem_Take *take = GetActiveTake(g_session.audio);
    const double pos = GetMediaItemInfo_Value(g_session.audio, "D_POSITION");
    const double len = GetMediaItemInfo_Value(g_session.audio, "D_LENGTH");
    const double offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    const auto setAudioTiming = [&] {
        g_session.audioPos = pos;
        g_session.audioLen = len;
        g_session.audioOffs = offs;
        g_session.audioRate = rate;
    };
    static double s_changedAt = -1; // re-analyze once the item has stopped changing for a moment
    if (const std::string id = audioIdentity(take); id != g_session.audioIdentity) {
        // another take or source: what is shown describes a different recording, so drop it now
        g_session.audioIdentity = id;
        setAudioTiming();
        cancelJob();
        clearAnalysis();
        updateValues();
        s_changedAt = -1;
        requestAnalysis(false);
    } else if (pos != g_session.audioPos || len != g_session.audioLen ||
               offs != g_session.audioOffs || rate != g_session.audioRate) {
        setAudioTiming();
        s_changedAt = time_precise();
        redraw();
    } else if (s_changedAt >= 0 && time_precise() - s_changedAt > REANALYSE_DELAY) {
        s_changedAt = -1;
        requestAnalysis(false);
    }

    // The takes are only looked at after a project change (edits, undo, moves, tempo changes all
    // count), and once a second for anything that doesn't: hashing four takes and walking their
    // tracks every tick costs more than the rest of the timer.
    const int change = GetProjectStateChangeCount(NULL);
    static int s_checkedChange = -1;
    static double s_checkedAt = 0;
    if (!g_session.ed.active() && (change != s_checkedChange || time_precise() - s_checkedAt > 1)) {
        s_checkedChange = change;
        s_checkedAt = time_precise();
        if (notesStale()) {
            loadNotes(); // edited elsewhere, undo, item moved, tempo changed, ...
        }
        if (!g_session.midi) {
            g_session.midi = VoiceItem(g_session.audio, g_session.voice, false);
            if (g_session.midi) {
                loadNotes(); // created elsewhere
            }
        }
        loadOtherVoices(false);
        loadReference();
    }
    updateIsolate();

    // redraw for the play and edit cursors, and for anything a project change may have changed
    const int state = GetPlayState();
    const double playPos = GetPlayPosition2();
    const double cursor = GetCursorPosition();
    if (state != g_lastPlayState || ((state & 1) && playPos != g_lastPlayPos) ||
        cursor != g_lastCursor || change != g_lastProjChange) {
        redraw();
    }
    g_lastPlayState = state;
    g_lastPlayPos = playPos;
    g_lastCursor = cursor;
    g_lastProjChange = change;
}
