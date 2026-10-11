// The window's logic that doesn't need REAPER: the settings and their text format, quantise and
// the suggestion rules (self-checked in tests/model_test.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "editor.h"
#include "nsgt.h"

// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };
// analysis channels; centre = mid minus side
enum { CH_MIX, CH_LEFT, CH_RIGHT, CH_SIDE, CH_CENTRE };

// Everything remembered per item (settings.cpp keeps it in the project): the defaults are here only.
struct Settings {
    NSGTParams np;
    double binsPerSemitone = 5; // slider for np.binsPerOctave, applied in requestAnalysis()
    RenderParams rp;
    int channel = CH_MIX;
    int snapMode = SNAP_ONSETS;
    double quantStrength = 1;
    bool ghosts = true; // faint outlines at each note's overtones
    double harmonics = 8, guideOpacity = 0.4;
    // Explain-away: dim spectrogram energy on drawn notes and their overtones, so what is still
    // bright is what still needs transcribing.
    bool explain = false;
    bool showContour = true, suggest = true, pitchFromContour = true;
    int suggestFrom = 0;      // SUGGEST_FROM
    bool estimateVote = true; // the estimator's front-end vote (EstimateParams::vote)
    bool audition = true;     // play a note through the notes track when clicked or drawn
    bool followTheme = true;  // else the built-in dark colours
    bool linkArrange = false; // keep the visible time range in sync with REAPER's arrange view
};

// "name=value;" pairs, as stored in the project.
std::string encodeSettings(const Settings &s);
// Unknown names are ignored and values clamped to their range (they come from a project file);
// what isn't mentioned keeps its value in `s`.
void decodeSettings(const char *str, Settings &s);

// Moves the selected notes' starts and ends towards snap() by `strength` (0..1), keeping each at
// least minLen long; true if any moved.
bool quantiseNotes(std::vector<Note> &notes, const std::function<double(double)> &snap,
                   double strength, double minLen);

// Whether a drawn note hides a suggestion at [start, end) and `pitch`: within a semitone (for
// corrected pitches) over a third of the suggestion's length.
bool coversSuggestion(const Note &n, double start, double end, double pitch);
