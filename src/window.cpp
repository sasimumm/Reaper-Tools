#include "ui.h"

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
    if (g_hwnd) {
        GetClientRect(g_hwnd, &r);
    }
    return {0, 0, (int)r.right, (int)r.bottom};
}

bool panelShown() { return g_panel && clientRect().r >= KB_W + PANEL_W + 100; }
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
    g_view.x0 = n.l, g_view.y0 = n.t;
}

void redraw() {
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}
static void toggleDock();

// --- navigation --------------------------------------------------------------------

static void fitItem() {
    syncViewOrigin();
    const Rect n = noteArea();
    g_view.pps = std::max(5.0, (n.r - n.l) / std::max(0.1, g_audioLen * 1.02));
    g_view.t0 = g_audioPos - 0.01 * g_audioLen;
    g_view.rowH = std::max(3.0, (double)(n.b - n.t) / (84 - 40 + 1)); // vocal range E2..C6
    g_view.pitchTop = 84.5;
    redraw();
}

static void zoomToSelection() {
    double a = 1e30, b = -1e30;
    int lo = 128, hi = -1;
    for (const Note &nt : g_ed.notes) {
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
    g_view.pps = std::min(3000.0, (n.r - n.l) / (len * 1.2));
    g_view.t0 = a - len * 0.1;
    g_view.rowH = std::min(60.0, std::max(3.0, (double)(n.b - n.t) / (hi - lo + 7)));
    g_view.pitchTop = hi + 3.5;
    redraw();
}

static void centreOn(double t) {
    const Rect n = noteArea();
    g_view.t0 = t - 0.5 * (n.r - n.l) / g_view.pps - (n.l - g_view.x0) / g_view.pps;
    redraw();
}

static void zoomTime(double factor, int x) {
    const double t = g_view.t(x);
    g_view.pps = std::min(3000.0, std::max(2.0, g_view.pps * factor));
    g_view.t0 = t - (x - g_view.x0) / g_view.pps;
    redraw();
}

static void zoomPitch(double factor, int y) {
    const double p = g_view.p(y);
    g_view.rowH = std::min(60.0, std::max(3.0, g_view.rowH * factor));
    g_view.pitchTop = p + (y - g_view.y0) / g_view.rowH;
    redraw();
}

static void scrollPitch(double semitones) {
    g_view.pitchTop = std::min(128.0, std::max(g_view.pitchTop + semitones,
                                               (noteArea().b - noteArea().t) / g_view.rowH - 0.5));
    redraw();
}

// Linked to the arrange view: whichever side changed since the last tick moves the other one.
// Starts by following the arrange view.
static double g_arrA = -1, g_arrB = -1, g_ourA, g_ourB; // ranges as of the last sync

static void syncArrange() {
    if (!g_linkArrange) {
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
        g_view.pps = (n.r - n.l) / (b - a), g_view.t0 = a;
        redraw();
    } else if (g_view.t(n.l) != g_ourA || g_view.t(n.r) != g_ourB) {
        a = g_view.t(n.l), b = g_view.t(n.r);
        GetSet_ArrangeView2(NULL, true, 0, 0, &a, &b);
        GetSet_ArrangeView2(NULL, false, 0, 0, &a, &b); // read back what REAPER made of it
    }
    g_arrA = a, g_arrB = b, g_ourA = g_view.t(n.l), g_ourB = g_view.t(n.r);
}

void selectVoice(int v) {
    if (v < 0 || v >= WINDOW_VOICES || v == g_voice) {
        return;
    }
    if (g_edit) {
        endLyric(true, 0);
    }
    g_ed.cancel();
    g_voice = g_voicePick = v;
    g_midi = VoiceItem(g_audio, v, false); // created with the first note
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
    g_np.gamma = std::max(3.0, std::min(40.0, g_np.gamma + delta));
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
        if (g_ed.deleteSelected()) {
            commitNotes();
        }
    } else if (key == VK_ESCAPE) {
        if (g_ed.active()) {
            g_ed.cancel(), g_drag = DRAG_NONE, ReleaseCapture();
        } else if (g_ed.selectAll(false)) {
            commitNotes();
        }
    } else if (key == 'A' && (m & MOD_CMD)) {
        if (g_ed.selectAll(true)) {
            commitNotes();
        }
    } else if (key == VK_RETURN) {
        int i = firstSelected();
        if (i < 0) {
            g_ed.hitTest(g_view, g_mouseX, g_mouseY,
                         &i); // nothing selected: the note under the mouse
        }
        beginLyric(i);
    } else if (key == 'H' && !m) {
        g_ghosts = !g_ghosts;
    } else if (key == 'S' && !m) {
        g_panel = !g_panel;
    } else if (key == 'G' && !m) {
        g_snapMode = (g_snapMode + 1) % 3, setupEditorSnap();
    } else if (key == 'P' && !m) {
        g_showContour = !g_showContour;
    } else if (key == 'X' && !m) {
        g_explain = !g_explain;
    } else if (key >= '1' && key < '1' + WINDOW_VOICES && !m) {
        selectVoice(key - '1');
    } else if (key == 'Q' && !m) {
        quantiseSelected();
    } else if (key == 'D' && !m) {
        g_dockPending = true;
    } else if (key == 'L' && !m) {
        g_linkArrange = !g_linkArrange;
    } else if (key == 'N' && !m) {
        g_suggest = !g_suggest;
    } else if (key == 'A' && !m) {
        acceptVisibleSuggestions();
    } else if (key == 'V' && (m & MOD_CMD)) {
        paste();
    } else if (key == 'C' && (m & MOD_CMD)) {
        copyNotes(false);
    } else if (key == 'X' && (m & MOD_CMD)) {
        copyNotes(true);
    } else if (key == 'D' && (m & MOD_CMD)) {
        if (g_ed.duplicateSelected()) {
            commitNotes();
        }
    } else if (key == 'K' && !m) {
        splitNotes();
    } else if (key == 'R' && !m) {
        loopPlay();
    } else if (key == 'B' && !m) {
        swapListen();
    } else if (key == 'J' && !m) {
        if (g_ed.joinSelected()) {
            commitNotes();
        }
    } else if (key == 'F' && !m) {
        fitItem();
    } else if (key == 'Z' && !m) {
        zoomToSelection();
    } else if (key == ('[' | CHARKEY)) {
        g_rp.brightness = std::max(-1.0, g_rp.brightness - 0.05), updateValues();
    } else if (key == (']' | CHARKEY)) {
        g_rp.brightness = std::min(1.0, g_rp.brightness + 0.05), updateValues();
    } else if (key == ('-' | CHARKEY)) {
        g_rp.contrast = std::max(0.1, g_rp.contrast / 1.1), updateValues();
    } else if (key == ('=' | CHARKEY)) {
        g_rp.contrast = std::min(3.0, g_rp.contrast * 1.1), updateValues();
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
    if (!g_hwnd || (msg->hwnd != g_hwnd && !IsChild(g_hwnd, msg->hwnd))) {
        return 0;
    }
    if (g_edit && msg->hwnd != g_hwnd) {
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
                GetWindowText(g_edit, buf, sizeof(buf) - 1);
                strlcat(buf, "-", sizeof(buf));
                SetWindowText(g_edit, buf);
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
        if (g_edit) {
            endLyric(true, 0); // clicking elsewhere finishes the lyric
        }
        SetFocus(hwnd);
        if (x < KB_W && y >= noteArea().t && y < noteArea().b) {
            previewPitch(g_view.pitchAt(y));
            return 0;
        }
        if (x < KB_W && y < RULER_H) {
            g_panel = !g_panel;
            redraw();
            return 0;
        }
        if (panelShown() && panelArea().in(x, y)) {
            g_drag = DRAG_PANEL, panelMouse(x, y, true);
        } else if (rulerArea().in(x, y) && (mods() & MOD_SHIFT)) {
            // shift+drag in the ruler: time selection, snapped like notes
            g_drag = DRAG_RULER_SELECT;
            g_selectAnchor = (mods() & MOD_ALT) ? g_view.t(x) : g_ed.snap(g_view.t(x));
            setSelection(g_selectAnchor, g_selectAnchor);
        } else if (rulerArea().in(x, y)) {
            g_drag = DRAG_RULER, SetEditCurPos(g_view.t(x), false, true);
        } else if (overviewArea().in(x, y)) {
            g_drag = DRAG_OVERVIEW, centreOn(overviewTime(x));
        } else if (lyricsArea().in(x, y)) {
            beginLyric(lyricAt(x));
            return 0;
        } else if (noteArea().in(x, y)) {
            int sug = -1;
            if (g_ed.hitTest(g_view, x, y, NULL) == NoteEditor::NONE &&
                !(mods() & (MOD_SHIFT | MOD_CMD)) && (sug = suggestionAt(x, y)) >= 0) {
                acceptSuggestion(sug); // click a suggested note to make it a note
                return 0;
            }
            g_drag = DRAG_NOTES, g_ed.mouseDown(g_view, x, y, mods());
            int hit = -1;
            if (g_ed.hitTest(g_view, x, y, &hit) != NoteEditor::NONE) {
                previewPitch(g_ed.notes[hit].pitch);
            }
            g_lastDragPitch = hit >= 0 ? g_ed.notes[hit].pitch : -1;
        } else {
            return 0;
        }
        SetCapture(hwnd);
        redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        g_mouseX = x, g_mouseY = y;
        if (g_drag == DRAG_NOTES) {
            g_ed.mouseMove(g_view, x, y, mods()), previewDragged();
        } else if (g_drag == DRAG_RULER) {
            SetEditCurPos(g_view.t(x), false, true);
        } else if (g_drag == DRAG_RULER_SELECT) {
            const double t = (mods() & MOD_ALT) ? g_view.t(x) : g_ed.snap(g_view.t(x));
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
        const int drawn = d == DRAG_NOTES ? g_ed.drawing() : -1;
        if (d == DRAG_NOTES && g_ed.mouseUp(g_view, x, y, mods())) {
            if (drawn >= 0 && drawn < (int)g_ed.notes.size() && g_pitchFromContour &&
                !(mods() & MOD_ALT)) {
                pitchFromContour(g_ed.notes[drawn]);
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
        if (noteArea().in(x, y) && g_ed.hitTest(g_view, x, y, &hit) != NoteEditor::NONE) {
            beginLyric(hit);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (noteArea().in(x, y) && !g_ed.active() && g_ed.deleteAt(g_view, x, y)) {
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
            g_view.t0 -= steps * 40 / g_view.pps;
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
        g_edit = NULL; // destroyed with the window
        if (!g_recreating) {
            cancelJob(), clearCache();
        }
        if (g_memdc) {
            SWELL_DeleteGfxContext(g_memdc);
        }
        g_memdc = NULL, g_memw = g_memh = 0, g_hwnd = NULL, g_layerValid = false,
        g_drag = DRAG_NONE;
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
        g_hwnd = CreateDialog(NULL, NULL, GetMainHwnd(), dlgProc);
        if (!g_hwnd) {
            return;
        }
        DockWindowAddEx(g_hwnd, "ReaTrace", "reatrace_editor", true);
        DockWindowActivate(g_hwnd);
    } else {
        // 0x400001: SWELL's "no dialog resource, resizable top-level window"
        g_hwnd = CreateDialog(NULL, (const char *)(INT_PTR)0x400001, GetMainHwnd(), dlgProc);
        if (!g_hwnd) {
            return;
        }
        SetWindowPos(g_hwnd, NULL, 80, 80, 1300, 760, SWP_NOZORDER);
    }
    char title[512];
    MediaItem_Take *take = GetActiveTake(g_audio);
    snprintf(title, sizeof(title), "ReaTrace - %s", take ? GetTakeName(take) : "");
    SetWindowText(g_hwnd, title);
    ShowWindow(g_hwnd, SW_SHOW);
    fitItem();
}

static void toggleDock() {
    const ViewMap view = g_view;
    g_recreating = true;
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
    }
    g_recreating = false;
    g_docked = !g_docked;
    SetExtState(EXT_SECTION, "docked", g_docked ? "1" : "0", true);
    createWindow();
    g_view = view;
    redraw();
}

void Window_Open(MediaItem *audio, int voice) {
    voice = std::max(0, std::min(WINDOW_VOICES - 1, voice));
    if (g_hwnd && audio == g_audio) {
        selectVoice(voice);
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        return;
    }
    Window_Close();
    clearCache();
    g_audio = audio, g_voice = g_voicePick = voice;
    g_midi = VoiceItem(audio, voice, false);
    for (OtherVoice &o : g_others) {
        o = OtherVoice();
    }
    g_audioPos = GetMediaItemInfo_Value(audio, "D_POSITION");
    g_audioLen = GetMediaItemInfo_Value(audio, "D_LENGTH");
    MediaItem_Take *take = GetActiveTake(audio);
    g_audioOffs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    g_audioRate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    g_spec = Spectrogram(), g_contour = Contour(), g_segments.clear(), g_onsets.clear(),
    g_snapTimes.clear();
    g_estimated.clear(), g_estimatedVoice.clear(), g_estimateNote.clear(), g_analysisError.clear();
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
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
    }
}

void Window_Timer() {
    stopPreviews(true);
    if (!g_hwnd) {
        return;
    }
    if (g_dockPending) {
        g_dockPending = false;
        toggleDock();
        if (!g_hwnd) {
            return;
        }
    }
    pollJob();
    syncArrange();
    if (g_drag == DRAG_NONE) {
        saveItemSettings();
    }
    if (!ValidatePtr2(NULL, g_audio, "MediaItem*")) {
        Window_Close();
        return;
    }

    // the item may have been moved, trimmed, slipped or sped up: re-analyze
    MediaItem_Take *take = GetActiveTake(g_audio);
    const double pos = GetMediaItemInfo_Value(g_audio, "D_POSITION"),
                 len = GetMediaItemInfo_Value(g_audio, "D_LENGTH");
    const double offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
    const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
    static double s_changedAt = -1; // re-analyze once the item has stopped changing for a moment
    if (pos != g_audioPos || len != g_audioLen || offs != g_audioOffs || rate != g_audioRate) {
        g_audioPos = pos, g_audioLen = len, g_audioOffs = offs, g_audioRate = rate;
        s_changedAt = time_precise();
        redraw();
    } else if (s_changedAt >= 0 && time_precise() - s_changedAt > 0.3) {
        s_changedAt = -1;
        requestAnalysis(false);
    }
    if (!g_ed.active() && takeHash(midiTake()) != g_hash) {
        loadNotes(); // edited elsewhere, undo, ...
    }
    if (!g_midi && !g_ed.active() && (g_midi = VoiceItem(g_audio, g_voice, false))) {
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
