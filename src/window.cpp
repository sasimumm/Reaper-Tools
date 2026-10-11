#include "window.h"

#include "state.h"

#include "analysis.h"
#include "commands.h"
#include "guard.h"
#include "lyrics.h"
#include "paint.h"
#include "panel.h"
#include "preview.h"
#include "settings.h"
#include "takesync.h"

// Zoom per pinch event: SWELL clamps each event's factor to 0.5..2, so only its direction is used.
static const double PINCH_STEP = 1.03;
static const double WHEEL_ZOOM = 1.05; // zoom per wheel notch (cmd/alt+scroll)
static bool g_docked;     // in REAPER's docker rather than a floating window (remembered)
static bool g_recreating; // destroying the window only to recreate it: keep the state

enum Drag { DRAG_NONE, DRAG_NOTES, DRAG_RULER, DRAG_RULER_SELECT, DRAG_PANEL, DRAG_OVERVIEW };
static double g_selectAnchor;
static Drag g_drag;

// change detection for the timer
static int g_lastPlayState, g_lastProjChange;
static double g_lastPlayPos, g_lastCursor;

Rect clientRect() {
    RECT r = {0, 0, 0, 0};
    if (g_ui.hwnd) {
        GetClientRect(g_ui.hwnd, &r);
    }
    return {0, 0, (int)r.right, (int)r.bottom};
}

bool panelShown() { return g_ui.panel && clientRect().r >= KB_W + PANEL_W + 100; }
Rect noteArea() {
    Rect c = clientRect();
    return {
        KB_W, RULER_H + CHORDS_H, std::max(KB_W + 1, c.r - (panelShown() ? PANEL_W : 0)),
        std::max(RULER_H + CHORDS_H + 1, c.b - STATUS_H - OVERVIEW_H - LYRICS_H)
    };
}
Rect rulerArea() {
    Rect n = noteArea();
    return {n.l, 0, n.r, RULER_H};
}
Rect chordsArea() {
    Rect n = noteArea();
    return {n.l, RULER_H, n.r, n.t};
}
Rect lyricsArea() {
    Rect n = noteArea();
    return {n.l, n.b, n.r, n.b + LYRICS_H};
}
Rect overviewArea() {
    Rect n = noteArea();
    return {n.l, n.b + LYRICS_H, n.r, n.b + LYRICS_H + OVERVIEW_H};
}
Rect panelArea() {
    Rect c = clientRect(), n = noteArea();
    return {n.r, 0, c.r, c.b};
}

void syncViewOrigin() {
    Rect n = noteArea();
    g_ui.view.x0 = n.l, g_ui.view.y0 = n.t;
}

void redraw() {
    if (g_ui.hwnd) {
        InvalidateRect(g_ui.hwnd, NULL, FALSE);
    }
}
static void toggleDock();

// --- navigation --------------------------------------------------------------------

static void fitItem() {
    syncViewOrigin();
    const Rect n = noteArea();
    g_ui.view.pps = std::max(5.0, (n.r - n.l) / std::max(0.1, g_session.audioLen * 1.02));
    g_ui.view.t0 = g_session.audioPos - 0.01 * g_session.audioLen;
    g_ui.view.rowH = std::max(3.0, (double)(n.b - n.t) / (84 - 40 + 1)); // vocal range E2..C6
    g_ui.view.pitchTop = 84.5;
    redraw();
}

static void zoomToSelection() {
    double a = 1e30, b = -1e30;
    int lo = 128, hi = -1;
    for (const Note &nt : g_session.ed.notes) {
        if (nt.sel) {
            a = std::min(a, nt.start), b = std::max(b, nt.end), lo = std::min(lo, nt.pitch),
            hi = std::max(hi, nt.pitch);
        }
    }
    if (hi < 0) {
        return;
    }
    const Rect n = noteArea();
    const double len = std::max(0.2, b - a);
    g_ui.view.pps = std::min(3000.0, (n.r - n.l) / (len * 1.2));
    g_ui.view.t0 = a - len * 0.1;
    g_ui.view.rowH = std::min(60.0, std::max(3.0, (double)(n.b - n.t) / (hi - lo + 7)));
    g_ui.view.pitchTop = hi + 3.5;
    redraw();
}

static void centreOn(double t) {
    const Rect n = noteArea();
    g_ui.view.t0 = t - 0.5 * (n.r - n.l) / g_ui.view.pps - (n.l - g_ui.view.x0) / g_ui.view.pps;
    redraw();
}

static void zoomTime(double factor, int x) {
    const double t = g_ui.view.t(x);
    g_ui.view.pps = std::min(3000.0, std::max(2.0, g_ui.view.pps * factor));
    g_ui.view.t0 = t - (x - g_ui.view.x0) / g_ui.view.pps;
    redraw();
}

static void zoomPitch(double factor, int y) {
    const double p = g_ui.view.p(y);
    g_ui.view.rowH = std::min(60.0, std::max(3.0, g_ui.view.rowH * factor));
    g_ui.view.pitchTop = p + (y - g_ui.view.y0) / g_ui.view.rowH;
    redraw();
}

static void scrollPitch(double semitones) {
    g_ui.view.pitchTop =
        std::min(128.0, std::max(g_ui.view.pitchTop + semitones,
                                 (noteArea().b - noteArea().t) / g_ui.view.rowH - 0.5));
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
        g_ui.view.pps = (n.r - n.l) / (b - a), g_ui.view.t0 = a;
        redraw();
    } else if (g_ui.view.t(n.l) != g_ourA || g_ui.view.t(n.r) != g_ourB) {
        a = g_ui.view.t(n.l), b = g_ui.view.t(n.r);
        GetSet_ArrangeView2(NULL, true, 0, 0, &a, &b);
        GetSet_ArrangeView2(NULL, false, 0, 0, &a, &b); // read back what REAPER made of it
    }
    g_arrA = a, g_arrB = b, g_ourA = g_ui.view.t(n.l), g_ourB = g_ui.view.t(n.r);
}

void selectVoice(int v) {
    if (v < 0 || v >= WINDOW_VOICES || v == g_session.voice) {
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
    return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) |
           ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_CMD : 0) |
           ((GetAsyncKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0);
}

static void adjustGamma(double delta) {
    g_settings.np.gamma = std::max(3.0, std::min(40.0, g_settings.np.gamma + delta));
    requestAnalysis(false);
}

// Punctuation arrives as a character code without FVIRTKEY, and some collide with VK codes
// ('.' == VK_DELETE), so keys are encoded as key | CHARKEY for characters.
static const int CHARKEY = 0x10000;
static int keyCode(int key, int flags) { return (flags & FVIRTKEY) ? key : key | CHARKEY; }

// returns true if handled
static bool onKey(int key) {
    const int m = mods();
    if (key == VK_DELETE || key == VK_BACK) {
        if (g_session.ed.deleteSelected()) {
            commitNotes();
        }
    } else if (key == VK_ESCAPE) {
        if (g_session.ed.active()) {
            g_session.ed.cancel(), g_drag = DRAG_NONE, ReleaseCapture();
        } else if (g_session.ed.selectAll(false)) {
            commitNotes();
        }
    } else if (key == 'A' && (m & MOD_CMD)) {
        if (g_session.ed.selectAll(true)) {
            commitNotes();
        }
    } else if (key == VK_RETURN) {
        int i = firstSelected();
        if (i < 0) {
            g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY,
                                 &i); // nothing selected: the note under the mouse
        }
        beginLyric(i);
    } else if (key == 'H' && !m) {
        g_settings.ghosts = !g_settings.ghosts;
    } else if (key == 'S' && !m) {
        g_ui.panel = !g_ui.panel;
    } else if (key == 'G' && !m) {
        g_settings.snapMode = (g_settings.snapMode + 1) % 3, setupEditorSnap();
    } else if (key == 'P' && !m) {
        g_settings.showContour = !g_settings.showContour;
    } else if (key == 'X' && !m) {
        g_settings.explain = !g_settings.explain;
    } else if (key >= '1' && key < '1' + WINDOW_VOICES && !m) {
        selectVoice(key - '1');
    } else if (key == 'Q' && !m) {
        quantiseSelected();
    } else if (key == 'D' && !m) {
        g_ui.dockPending = true;
    } else if (key == 'L' && !m) {
        g_settings.linkArrange = !g_settings.linkArrange;
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
        g_settings.rp.brightness = std::max(-1.0, g_settings.rp.brightness - 0.05), updateValues();
    } else if (key == (']' | CHARKEY)) {
        g_settings.rp.brightness = std::min(1.0, g_settings.rp.brightness + 0.05), updateValues();
    } else if (key == ('-' | CHARKEY)) {
        g_settings.rp.contrast = std::max(0.1, g_settings.rp.contrast / 1.1), updateValues();
    } else if (key == ('=' | CHARKEY)) {
        g_settings.rp.contrast = std::min(3.0, g_settings.rp.contrast * 1.1), updateValues();
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
    case '.' | CHARKEY: return true;
    case 'A': return true;
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
    case 'K':
    case 'J':
    case 'R':
    case 'B': return !cmd && !(flags & (FALT | FSHIFT));
    }
    return false;
}

int Window_TranslateAccel(MSG *msg) {
    if (!g_ui.hwnd || (msg->hwnd != g_ui.hwnd && !IsChild(g_ui.hwnd, msg->hwnd))) {
        return 0;
    }
    if (g_ui.edit && msg->hwnd != g_ui.hwnd) {
        // typing a lyric: Tab/Space next, shift+Tab previous, Enter done, Esc cancel; the rest goes to the field
        if (msg->message == WM_KEYDOWN) {
            const int key = (int)msg->wParam, flags = (int)msg->lParam;
            if (key == VK_TAB) {
                endLyric(true, (flags & FSHIFT) ? -1 : 1);
                return 1;
            }
            if (key == VK_SPACE && (flags & FVIRTKEY)) {
                endLyric(true, 1);
                return 1;
            }
            if (key == '-' && !(flags & FVIRTKEY)) // hyphen: this syllable joins the next one
            {
                char buf[1024] = "";
                GetWindowText(g_ui.edit, buf, sizeof(buf) - 1);
                strlcat(buf, "-", sizeof(buf));
                SetWindowText(g_ui.edit, buf);
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
    // Our keys are handled here: delivered to the window, SWELL would turn Enter/Esc into dialog commands.
    if (msg->message == WM_KEYDOWN &&
        isOurKey(keyCode((int)msg->wParam, (int)msg->lParam), (int)msg->lParam)) {
        onKey(keyCode((int)msg->wParam, (int)msg->lParam));
        return 1;
    }
    if (msg->message == WM_KEYUP &&
        isOurKey(keyCode((int)msg->wParam, (int)msg->lParam), (int)msg->lParam)) {
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
    case WM_KEYDOWN: return onKey(keyCode((int)wParam, (int)lParam)) ? 0 : 1;
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
        if (x < KB_W && y < RULER_H) {
            g_ui.panel = !g_ui.panel;
            redraw();
            return 0;
        }
        if (panelShown() && panelArea().in(x, y)) {
            g_drag = DRAG_PANEL, panelMouse(x, y, true);
        } else if (rulerArea().in(x, y) && (mods() & MOD_SHIFT)) {
            // shift+drag in the ruler: time selection, snapped like notes
            g_drag = DRAG_RULER_SELECT;
            g_selectAnchor =
                (mods() & MOD_ALT) ? g_ui.view.t(x) : g_session.ed.snap(g_ui.view.t(x));
            setSelection(g_selectAnchor, g_selectAnchor);
        } else if (rulerArea().in(x, y)) {
            g_drag = DRAG_RULER, SetEditCurPos(g_ui.view.t(x), false, true);
        } else if (overviewArea().in(x, y)) {
            g_drag = DRAG_OVERVIEW, centreOn(overviewTime(x));
        } else if (lyricsArea().in(x, y)) {
            beginLyric(lyricAt(x));
            return 0;
        } else if (noteArea().in(x, y)) {
            int sug = -1;
            if (g_session.ed.hitTest(g_ui.view, x, y, NULL) == NoteEditor::NONE &&
                !(mods() & (MOD_SHIFT | MOD_CMD)) && (sug = suggestionAt(x, y)) >= 0) {
                acceptSuggestion(sug); // click a suggested note to make it a note
                return 0;
            }
            g_drag = DRAG_NOTES, g_session.ed.mouseDown(g_ui.view, x, y, mods());
            int hit = -1;
            if (g_session.ed.hitTest(g_ui.view, x, y, &hit) != NoteEditor::NONE) {
                previewPitch(g_session.ed.notes[hit].pitch);
            }
            previewDragStarted(hit >= 0 ? g_session.ed.notes[hit].pitch : -1);
        } else {
            return 0;
        }
        SetCapture(hwnd);
        redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        g_ui.mouseX = x, g_ui.mouseY = y;
        if (g_drag == DRAG_NOTES) {
            g_session.ed.mouseMove(g_ui.view, x, y, mods()), previewDragged();
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
        const int drawn = d == DRAG_NOTES ? g_session.ed.drawing() : -1;
        if (d == DRAG_NOTES && g_session.ed.mouseUp(g_ui.view, x, y, mods())) {
            if (drawn >= 0 && drawn < (int)g_session.ed.notes.size() &&
                g_settings.pitchFromContour && !(mods() & MOD_ALT)) {
                pitchFromContour(g_session.ed.notes[drawn]);
            }
            commitNotes();
        }
        if (d == DRAG_PANEL) {
            finishPanelDrag();
        }
        redraw();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // double-click a note to type its lyric
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
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
        // trackpad friendly: scroll pans, cmd+scroll zooms time, alt+scroll zooms pitch, shift+scroll pans time
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        syncViewOrigin();
        const double steps = (short)HIWORD(wParam) / 120.0;
        const int m = mods();
        if (panelShown() && panelArea().in(pt.x, pt.y)) {
            if (msg == WM_MOUSEWHEEL) {
                scrollPanel((int)lround(-steps * 30));
            }
            return 0;
        }
        if (msg == WM_MOUSEHWHEEL || ((m & MOD_SHIFT) && !(m & (MOD_CMD | MOD_ALT)))) {
            g_ui.view.t0 -= steps * 40 / g_ui.view.pps;
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
        GESTUREINFO *gi = (GESTUREINFO *)lParam;
        if (gi && gi->dwID == GID_ZOOM && (gi->dwFlags & GF_END) && gi->ullArguments) {
            POINT pt = {gi->ptsLocation.x, gi->ptsLocation.y};
            ScreenToClient(hwnd, &pt);
            syncViewOrigin();
            const double f = gi->ullArguments > 1024   ? PINCH_STEP
                             : gi->ullArguments < 1024 ? 1 / PINCH_STEP
                                                       : 1;
            if (mods() & MOD_ALT) {
                zoomPitch(f, pt.y);
            } else {
                zoomTime(f, pt.x);
            }
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (g_docked) {
            DockWindowRemove(hwnd);
        }
        saveItemSettings();
        stopPreviews(false);
        g_ui.edit = NULL; // destroyed with the window
        if (!g_recreating) {
            cancelJob(), clearCache();
        }
        releasePaintCache();
        g_ui.hwnd = NULL, g_drag = DRAG_NONE;
        return 0;
    }
    return 0;
}

static INT_PTR WINAPI dlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return guarded("window", (INT_PTR)0, [&] { return handleMessage(hwnd, msg, wParam, lParam); });
}

// --- lifecycle ---------------------------------------------------------------------

// Floating: a resizable top-level window. Docked: a child window handed to REAPER's docker
// (SWELL makes a child view when there is no dialog resource and no 0x400001 style).
static void createWindow() {
    if (g_docked) {
        g_ui.hwnd = CreateDialog(NULL, NULL, GetMainHwnd(), dlgProc);
        if (!g_ui.hwnd) {
            return;
        }
        DockWindowAddEx(g_ui.hwnd, "ReaTrace", "reatrace_editor", true);
        DockWindowActivate(g_ui.hwnd);
    } else {
        // 0x400001: SWELL's "no dialog resource, resizable top-level window"
        g_ui.hwnd = CreateDialog(NULL, (const char *)(INT_PTR)0x400001, GetMainHwnd(), dlgProc);
        if (!g_ui.hwnd) {
            return;
        }
        SetWindowPos(g_ui.hwnd, NULL, 80, 80, 1300, 760, SWP_NOZORDER);
    }
    char title[512];
    MediaItem_Take *take = GetActiveTake(g_session.audio);
    snprintf(title, sizeof(title), "ReaTrace - %s", take ? GetTakeName(take) : "");
    SetWindowText(g_ui.hwnd, title);
    ShowWindow(g_ui.hwnd, SW_SHOW);
    fitItem();
}

static void toggleDock() {
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
    g_session.audio = audio, g_session.voice = g_ui.voicePick = voice;
    g_session.midi = VoiceItem(audio, voice, false);
    g_session.audioPos = GetMediaItemInfo_Value(audio, "D_POSITION");
    g_session.audioLen = GetMediaItemInfo_Value(audio, "D_LENGTH");
    MediaItem_Take *take = GetActiveTake(audio);
    g_session.audioOffs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    g_session.audioRate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    clearAnalysis();
    const bool hadSettings = loadItemSettings();
    updateValues();
    requestAnalysis(!hadSettings);
    setupEditorSnap();
    loadNotes();
    loadOtherVoices(true);

    const char *d = globalState("docked");
    g_docked = d && *d == '1';
    createWindow();
}

void Window_Close() {
    if (g_ui.hwnd) {
        DestroyWindow(g_ui.hwnd);
    }
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
    if (g_drag == DRAG_NONE) {
        saveItemSettings();
    }
    if (!ValidatePtr2(NULL, g_session.audio, "MediaItem*")) {
        Window_Close();
        return;
    }

    // the item may have been moved, trimmed, slipped or sped up: re-analyze
    MediaItem_Take *take = GetActiveTake(g_session.audio);
    const double pos = GetMediaItemInfo_Value(g_session.audio, "D_POSITION"),
                 len = GetMediaItemInfo_Value(g_session.audio, "D_LENGTH");
    const double offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    static double s_changedAt = -1; // re-analyze once the item has stopped changing for a moment
    if (pos != g_session.audioPos || len != g_session.audioLen || offs != g_session.audioOffs ||
        rate != g_session.audioRate) {
        g_session.audioPos = pos, g_session.audioLen = len, g_session.audioOffs = offs,
        g_session.audioRate = rate;
        s_changedAt = time_precise();
        redraw();
    } else if (s_changedAt >= 0 && time_precise() - s_changedAt > 0.3) {
        s_changedAt = -1;
        requestAnalysis(false);
    }
    if (!g_session.ed.active() && takeHash(midiTake()) != g_session.hash) {
        loadNotes(); // edited elsewhere, undo, ...
    }
    if (!g_session.midi && !g_session.ed.active() &&
        (g_session.midi = VoiceItem(g_session.audio, g_session.voice, false))) {
        loadNotes(); // created elsewhere
    }
    loadOtherVoices(false);

    const int state = GetPlayState(), change = GetProjectStateChangeCount(NULL);
    const double playPos = GetPlayPosition2(), cursor = GetCursorPosition();
    if (state != g_lastPlayState || ((state & 1) && playPos != g_lastPlayPos) ||
        cursor != g_lastCursor || change != g_lastProjChange) {
        redraw();
    }
    g_lastPlayState = state, g_lastPlayPos = playPos, g_lastCursor = cursor,
    g_lastProjChange = change;
}
