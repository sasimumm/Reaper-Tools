// The window's logic that doesn't need REAPER: the settings and their text format, the layout,
// quantise, the suggestion rules and follow-playback paging (self-checked in tests/model_test.cpp).
#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dsp/nsgt.h"
#include "notes/editor.h"

// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };

// analysis channels; centre = mid minus side
enum { CH_MIX, CH_LEFT, CH_RIGHT, CH_SIDE, CH_CENTRE };

// Everything remembered per item (settings.cpp keeps it in the project): the defaults are here
// only.
struct Settings {
    NSGTParams np;
    double binsPerSemitone = 5; // slider for np.binsPerOctave, applied in requestAnalysis()
    RenderParams rp;
    int channel = CH_MIX;
    int snapMode = SNAP_ONSETS;
    double quantStrength = 1;                 // 0..1, see quantiseNotes()
    bool ghosts = true;                       // faint outlines at each note's overtones
    double harmonics = 8, guideOpacity = 0.4; // overtones drawn per note; their opacity 0..1
    // Explain-away: dim spectrogram energy on drawn notes and their overtones, so what is still
    // bright is what still needs transcribing.
    bool explain = false;
    bool showContour = true, suggest = true, pitchFromContour = true;
    int suggestFrom = 0;      // SUGGEST_FROM
    bool estimateVote = true; // the estimator's front-end vote (EstimateParams::vote)
    bool audition = true;     // play a note through the notes track when clicked or drawn
    bool followTheme = true;  // else the built-in dark colours
    bool linkArrange = false; // keep the visible time range in sync with REAPER's arrange view
    bool followPlay = true;   // page the view along with the play cursor
    double isolateWidth = 100, isolateBackground = -24; // the isolate filter: cents, dB
    bool isolateAll = false;    // isolate every shown voice, not just the edited one
    bool isolateRemove = false; // the filter lowers the drawn notes instead (toggleIsolate)
};

// --- layout, in points ------------------------------------------------------------------

// Widths (_W) and heights (_H) of the window's areas.
static const int KB_W = 56; // keyboard column
static const int TOOLBAR_H = 34;
static const int RULER_H = 30;
static const int STATUS_H = 22;
static const int PANEL_W = 250; // settings panel
static const int LYRICS_H = 18;
static const int OVERVIEW_H = 28;
static const int CHORDS_H = 20;
// Below this size only the background is drawn, so the areas always fit in the client rect.
static const int MIN_W = KB_W + 40;
static const int MIN_H = TOOLBAR_H + RULER_H + CHORDS_H + LYRICS_H + OVERVIEW_H + STATUS_H + 20;

// A rectangle in points: left, top inclusive; right, bottom exclusive.
struct Rect {
    int l, t, r, b;
    bool in(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
};

// Top to bottom: toolbar (full width), ruler, chords lane, notes (keyboard to their left), lyrics
// lane, overview, status line (full width); the settings panel on the right, under the toolbar,
// when there is room for it.
struct Layout {
    Rect client, toolbar, ruler, chords, notes, lyrics, overview, status, panel;
    bool panelShown; // else `panel` is empty and the notes reach the right edge
    bool tooSmall;   // below MIN_W x MIN_H: only the background is drawn
};
// Lanes that can be folded to a thin strip by clicking their label (bits of `folded`).
enum { LANE_CHORDS = 1, LANE_LYRICS = 2, LANE_OVERVIEW = 4 };
static const int FOLDED_H = 6;

// The layout of a client area width x height points, with the panel if wanted and there is room,
// and the lanes in `folded` (LANE_*) folded.
Layout computeLayout(int width, int height, bool panelWanted, unsigned folded = 0);

// --- settings ------------------------------------------------------------------------------

// "name=value;" pairs, as stored in the project.
std::string encodeSettings(const Settings &s);
// Unknown names are ignored and values clamped to their range (they come from a project file);
// what isn't mentioned keeps its value in `s`.
void decodeSettings(const char *str, Settings &s);

// Moves the selected notes' starts and ends towards snap() by `strength` (0..1), keeping each at
// least minLen long (all in project seconds); true if any moved.
bool quantiseNotes(std::span<Note> notes, const std::function<double(double)> &snap,
                   double strength, double minLen);

// Whether a drawn note hides a suggestion at [start, end) seconds and `pitch` (semitones): within
// a semitone (for corrected pitches) over a third of the suggestion's length.
bool coversSuggestion(const Note &n, double start, double end, double pitch);

// Follow playback: the view's new left edge when the play cursor went from `prev` (inside the view
// [t0, t0 + width), in seconds) to `now` outside it, else t0. The cursor lands a twentieth of the
// way in.
// A cursor already outside on the previous tick (the view scrolled away) is left alone.
double followPage(double t0, double width, double prev, double now);

// The isolate filter's pitch changes: (start seconds, pitch) of the first `max` spans by start that
// end after `from`.
std::vector<std::pair<double, int>> pitchSchedule(std::vector<Note> spans, double from, size_t max);
