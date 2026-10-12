#include "notes/commands.h"

#include "analysis.h"
#include "notes/lyrics.h"
#include "notes/takesync.h"
#include "playback/preview.h"

static const double ONSET_SNAP_RANGE = 0.03; // seconds
const std::vector<Segment> &suggestions() {
    return g_settings.suggestFrom ? g_analysis.segments : g_analysis.estimated;
}
// The voice a suggestion goes to with A: the estimator's assignment; contour suggestions are the active voice's.
int suggestionVoice(size_t i) {
    return g_settings.suggestFrom || i >= g_analysis.estimatedVoice.size()
               ? g_session.voice
               : g_analysis.estimatedVoice[i];
}

// --- grid ------------------------------------------------------------------------

// Finest straight division (in quarter notes) at least 10 px apart, or 0 for "measures".
double gridStepQN() {
    const double tMid = g_ui.view.t(g_ui.view.x0 + 200), qn = TimeMap2_timeToQN(NULL, tMid);
    for (double step : {0.125, 0.25, 0.5, 1.0}) {
        if ((TimeMap2_QNToTime(NULL, qn + step) - TimeMap2_QNToTime(NULL, qn)) * g_ui.view.pps >=
            10) {
            return step;
        }
    }
    return 0;
}

static double snapToGrid(double t) {
    const double qn = TimeMap2_timeToQN(NULL, t), step = gridStepQN();
    if (step > 0) {
        return TimeMap2_QNToTime(NULL, floor(qn / step + 0.5) * step);
    }
    double ms = 0, me = 0;
    TimeMap_QNToMeasures(NULL, qn, &ms, &me);
    return TimeMap2_QNToTime(NULL, qn - ms < me - qn ? ms : me);
}

// Arrow keys: left/right one grid step (a bar when zoomed out to bars), up/down a semitone.
void nudgeSelected(int steps, int semitones) {
    const auto move = [steps](double t) {
        const double qn = TimeMap2_timeToQN(NULL, t);
        double step = gridStepQN();
        if (step <= 0) {
            double ms = 0, me = 0;
            TimeMap_QNToMeasures(NULL, qn, &ms, &me);
            step = me - ms;
        }
        return TimeMap2_QNToTime(NULL, qn + steps * step);
    };
    if (g_session.ed.nudgeSelected(move, semitones)) {
        if (semitones != 0) {
            previewPitch(g_session.ed.notes[firstSelected()].pitch);
        }
        commitNotes();
    }
}

// --- quantise -------------------------------------------------------------------------

// Moves selected notes' starts and ends towards the grid by the quantise strength.
void quantiseSelected() {
    if (quantiseNotes(g_session.ed.notes, snapToGrid, g_settings.quantStrength,
                      g_session.ed.minLen)) {
        commitNotes();
    }
}

// --- contour and suggestions -------------------------------------------------------------

// Suggestions can overlap (harmonies), so one is hidden only by a drawn note in any voice at nearly
// the same pitch (within a semitone, for corrected pitches) over a third of its length.
bool covered(const Segment &sg) {
    const double a = frameTime(sg.f0), b = frameTime(sg.f1 + 1);
    for (const Note &n : g_session.ed.notes) {
        if (coversSuggestion(n, a, b, sg.pitch)) {
            return true;
        }
    }
    for (int v = 0; v < WINDOW_VOICES; v++) {
        for (const Note &n : g_session.others[v].notes) {
            if (v != g_session.voice && coversSuggestion(n, a, b, sg.pitch)) {
                return true;
            }
        }
    }
    return false;
}

void segmentRect(const Segment &sg, int *x0, int *y0, int *x1, int *y1) {
    const int p = (int)lround(sg.pitch);
    *x0 = (int)lround(g_ui.view.x(frameTime(sg.f0))),
    *x1 = std::max(*x0 + 2, (int)lround(g_ui.view.x(frameTime(sg.f1 + 1))));
    *y0 = (int)lround(g_ui.view.y(p + 0.5)),
    *y1 = std::max(*y0 + 3, (int)lround(g_ui.view.y(p - 0.5)));
}

bool suggestionHidden(size_t i) {
    const int v = suggestionVoice(i);
    return v >= 0 && (g_ui.hiddenVoices >> v & 1);
}

int suggestionAt(int x, int y) {
    if (!g_settings.suggest) {
        return -1;
    }
    const std::vector<Segment> &sug = suggestions();
    for (size_t i = 0; i < sug.size(); i++) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        segmentRect(sug[i], &x0, &y0, &x1, &y1);
        if (x >= x0 - 2 && x < x1 + 2 && y >= y0 - 2 && y < y1 + 2 && !suggestionHidden(i) &&
            !covered(sug[i])) {
            return (int)i;
        }
    }
    return -1;
}

int firstSelected() {
    int best = -1;
    for (size_t i = 0; i < g_session.ed.notes.size(); i++) {
        if (g_session.ed.notes[i].sel &&
            (best < 0 || g_session.ed.notes[i].start < g_session.ed.notes[best].start)) {
            best = (int)i;
        }
    }
    return best;
}

static double snapToOnsets(double t) {
    auto it = std::lower_bound(g_analysis.snapTimes.begin(), g_analysis.snapTimes.end(), t);
    double best = t, dist = ONSET_SNAP_RANGE;
    if (it != g_analysis.snapTimes.end() && *it - t <= dist) {
        best = *it, dist = *it - t;
    }
    if (it != g_analysis.snapTimes.begin() && t - *(it - 1) < dist) {
        best = *(it - 1);
    }
    return best;
}

void setupEditorSnap() {
    if (g_settings.snapMode == SNAP_GRID) {
        g_session.ed.snap = snapToGrid;
    } else if (g_settings.snapMode == SNAP_ONSETS) {
        g_session.ed.snap = snapToOnsets;
    } else {
        g_session.ed.snap = [](double t) { return t; };
    }
}

void acceptSuggestion(int i) {
    const Segment &sg = suggestions()[i];
    for (Note &o : g_session.ed.notes) {
        o.sel = false;
    }
    g_session.ed.notes.push_back(
        {frameTime(sg.f0), frameTime(sg.f1 + 1), (int)lround(sg.pitch), true, -1});
    previewPitch((int)lround(sg.pitch));
    commitNotes();
}

// A: every suggestion in view into its voice, as one undo point. Crowded-out ones (grey) are left.
void acceptVisibleSuggestions() {
    const Rect n = noteArea();
    std::vector<Note> byVoice[WINDOW_VOICES];
    const std::vector<Segment> &sug = suggestions();
    for (size_t i = 0; i < sug.size(); i++) {
        const Segment &sg = sug[i];
        const double a = frameTime(sg.f0), b = frameTime(sg.f1 + 1);
        const int v = suggestionVoice(i);
        if (v < 0 || suggestionHidden(i) || b < g_ui.view.t(n.l) || a > g_ui.view.t(n.r) ||
            covered(sg)) {
            continue;
        }
        byVoice[v].push_back({a, b, (int)lround(sg.pitch), false, -1});
    }
    bool others = false;
    for (int v = 0; v < WINDOW_VOICES; v++) {
        others |= v != g_session.voice && !byVoice[v].empty();
    }
    if (others) {
        Undo_BeginBlock2(NULL);
    }
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (v != g_session.voice && !byVoice[v].empty()) {
            insertIntoVoice(v, byVoice[v]);
        }
    }
    if (!byVoice[g_session.voice].empty()) {
        g_session.ed.notes.insert(g_session.ed.notes.end(), byVoice[g_session.voice].begin(),
                                  byVoice[g_session.voice].end());
        commitNotes();
    }
    if (others) {
        Undo_EndBlock2(NULL, "ReaTrace: accept suggested notes", UNDO_STATE_ITEMS);
        UpdateArrange();
        loadOtherVoices(true);
    }
}

// A freshly drawn note takes the median sung pitch under it when that is within two semitones.
void pitchFromContour(Note &nt) {
    const double fr = g_analysis.spec.frameRate();
    double sung = 0;
    if (contourMedian(g_analysis.contour, (int)lround((nt.start - g_session.audioPos) * fr),
                      (int)lround((nt.end - g_session.audioPos) * fr), (int)lround(0.05 * fr),
                      &sung) &&
        fabs(sung - nt.pitch) <= 2) {
        nt.pitch = (int)lround(sung);
    }
}

// cmd+V pastes notes after a copy here, until other text is copied (the system clipboard's text
// at copy time is remembered; it isn't overwritten). Otherwise it pastes lyrics.
static std::vector<Note> g_noteClip;
static std::string g_noteClipText;

void copyNotes(bool cut) {
    std::vector<Note> clip = g_session.ed.copySelected();
    if (clip.empty()) {
        return;
    }
    g_noteClip = clip, g_noteClipText = clipboardText();
    if (cut && g_session.ed.deleteSelected()) {
        commitNotes();
    }
}

void paste() {
    const std::string text = clipboardText();
    if (g_noteClip.empty() || text != g_noteClipText) {
        pasteLyrics(text);
        return;
    }
    g_session.ed.paste(g_noteClip, GetCursorPosition());
    commitNotes();
}

// Time selection and loop points together, whether or not REAPER links them.
void setSelection(double a, double b) {
    GetSet_LoopTimeRange2(NULL, true, false, &a, &b, false);
    GetSet_LoopTimeRange2(NULL, true, true, &a, &b, false);
}

// R: loop the selected notes' range (else the time selection) from its start; again to stop.
void loopPlay() {
    if (GetPlayState() & 1) {
        OnStopButton();
        return;
    }
    double a = 1e30, b = -1e30;
    for (const Note &nt : g_session.ed.notes) {
        if (nt.sel) {
            a = std::min(a, nt.start), b = std::max(b, nt.end);
        }
    }
    if (b > a) {
        setSelection(a, b);
    }
    GetSet_LoopTimeRange2(NULL, false, false, &a, &b, false);
    if (b <= a) {
        return;
    }
    GetSetRepeat(1);
    SetEditCurPos(a, false, false);
    OnPlayButton();
}

// Split: the note under the mouse at the (snapped) mouse time, else the selected notes there; with
// the mouse outside the notes, the selected notes at the edit cursor.
void splitNotes() {
    const Rect n = noteArea();
    int hit = -1;
    const bool inArea = n.in(g_ui.mouseX, g_ui.mouseY);
    if (inArea) {
        g_session.ed.hitTest(g_ui.view, g_ui.mouseX, g_ui.mouseY, &hit);
    }
    const double t = inArea ? ((mods() & MOD_ALT) ? g_ui.view.t(g_ui.mouseX)
                                                  : g_session.ed.snap(g_ui.view.t(g_ui.mouseX)))
                            : GetCursorPosition();
    if (g_session.ed.splitAt(t, hit)) {
        commitNotes();
    }
}
