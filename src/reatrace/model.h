// The window's logic that doesn't need REAPER: the settings and their text format, the layout,
// quantise, the suggestion rules and follow-playback paging (self-checked in tests/model_test.cpp).
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dsp/nsgt.h"
#include "notes/editor.h"

// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };

// analysis channels; centre = mid minus side
enum { CH_MIX, CH_LEFT, CH_RIGHT, CH_SIDE, CH_CENTRE };

// where suggestions come from: the note estimator, the lead's contour, reference MIDI tracks
enum { FROM_ESTIMATOR, FROM_CONTOUR, FROM_REFERENCE };

// Everything remembered per item (settings.cpp keeps it in the project): the defaults are here
// only.
struct Settings {
    NSGTParams np;
    double binsPerSemitone = 5; // slider for np.binsPerOctave, applied in requestAnalysis()
    RenderParams rp;
    int channel = CH_MIX;
    int snapMode = SNAP_ONSETS;
    // grid notes per whole note (4: quarters), 0 by zoom, 1 a measure; triplets: 2/3 of each step
    int gridDiv = 0;
    bool gridTriplets = false;
    double quantStrength = 1;                 // 0..1, see quantiseNotes()
    bool ghosts = true;                       // faint outlines at each note's overtones
    double harmonics = 8, guideOpacity = 0.4; // overtones drawn per note; their opacity 0..1
    // Explain-away: dim spectrogram energy on drawn notes and their overtones, so what is still
    // bright is what still needs transcribing.
    bool explain = false;
    bool chordTones = false; // shade each chord's notes under it (C)
    bool showContour = true, suggest = true;
    int suggestFrom = FROM_ESTIMATOR;
    // FROM_REFERENCE: the reference tracks' GUIDs, space separated; in order they suggest notes
    // for the lead, voice 2, ... (a single track for the edited voice)
    std::string reference;
    double referenceOpacity = 1; // of the reference tracks' suggestions, 0..1
    bool estimateVote = true;    // the estimator's front-end vote (EstimateParams::vote)
    bool audition = true;        // play a note through the notes track when clicked or drawn
    bool followTheme = true;     // else the built-in dark colours
    bool linkArrange = false;    // keep the visible time range in sync with REAPER's arrange view
    bool followPlay = true;      // page the view along with the play cursor
    double isolateWidth = 100, isolateBackground = -24; // the isolate filter: cents, dB
    bool isolateAll = false;  // isolate every shown voice, not just the edited one
    bool isolateDuck = false; // the filter lowers the drawn notes instead (toggleIsolate)
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

// Shift+I, after Dorico's add intervals popover: comma-separated intervals such as 3, -3, m3, P5,
// aug4 or -min6, where a negative degree goes down and a quality (M/maj, m/min, P/perf, d/dim,
// a/aug) makes it chromatic; without one it counts steps of the scale. A leading t transposes the
// selected notes by the first interval instead of adding notes.
enum IntervalQuality { IQ_DIATONIC, IQ_MAJOR, IQ_MINOR, IQ_PERFECT, IQ_DIMINISHED, IQ_AUGMENTED };
struct Interval {
    int degree; // 1 = unison, 3 = third, ...; negative below
    IntervalQuality quality;
};
struct IntervalInput {
    bool transpose = false;
    std::vector<Interval> intervals;
};
std::optional<IntervalInput> parseIntervals(std::string_view text);
// The pitch an interval away, -1 outside 0..127. root 0 = C; scale bit n = pitch class root + n,
// as MIDI_GetScale gives it; 0 means major. A pitch outside the scale keeps its distance above the
// scale note below it.
int intervalPitch(int pitch, Interval iv, int root, int scale);
// Adds each interval as a note above the highest selected note starting at a time (below the
// lowest, for negative ones), as long as that note, and selects just the added notes; or
// transposes the selected notes, unless one would leave 0..127. True if notes changed.
bool addIntervals(std::vector<Note> &notes, const IntervalInput &in, int root, int scale);

// A chord name's notes, as ReaChords and Logic write them: C, Cm, C7, Cmaj7, Cm7b5, Cdim7, C+,
// Csus4, C6/9, Cadd9, C13, C/E, ... root and bass are pitch classes (0 = C; bass -1 without a
// slash), tones a bit per pitch class above the root. Nothing for N.C. or a name it can't read.
struct ChordTones {
    int root = 0, bass = -1;
    unsigned tones = 0;
};
std::optional<ChordTones> chordTones(std::string_view name);

// A line of a project file's <KEYSIG> block, "<position> <root> <flags> <scale>" (as in
// "0 4 1 0xAB5", E major): the root (0 = C) and the scale as MIDI_GetScale gives it.
std::optional<std::pair<int, int>> keySigLine(std::string_view line);

// The isolate filter's pitch changes: (start seconds, pitch) of the first `max` spans by start that
// end after `from`.
std::vector<std::pair<double, int>> pitchSchedule(std::vector<Note> spans, double from, size_t max);
