// The ReaTrace window's state and layout, shared by its source files; each module's functions
// are in its own header (analysis.h, paint.h, panel.h, ...).
#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "reaper_plugin_functions.h"
#undef min
#undef max

#include "contour.h"
#include "editor.h"
#include "nsgt.h"

// Layout, in pixels.
static const int KB_W = 56, RULER_H = 30, STATUS_H = 20, PANEL_W = 250, LYRICS_H = 18,
                 OVERVIEW_H = 28, CHORDS_H = 20;
// --- state ---------------------------------------------------------------------

struct OtherVoice {
    MediaItem *item = NULL;
    std::string hash;
    std::vector<Note> notes;
};
// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };


enum { CH_MIX, CH_LEFT, CH_RIGHT, CH_SIDE, CH_CENTRE };

// Everything remembered per item (settings.cpp saves and loads it): the defaults are here only.
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
inline Settings g_settings;

// The item being transcribed and the active voice's notes (window.cpp opens it, takesync.cpp syncs it).
struct Session {
    MediaItem *audio = NULL, *midi = NULL; // midi: the active voice's notes item
    int voice = 0;                         // active voice, 0 = lead
    OtherVoice others[WINDOW_VOICES];
    double audioPos = 0, audioLen = 0, audioOffs = 0, audioRate = 1; // as analysed
    NoteEditor ed;
    std::string hash;     // the take's MIDI hash when the notes were loaded
    int notesVersion = 0; // bumped whenever notes are loaded
};
inline Session g_session;

// The installed analysis of the item (written only by analysis.cpp's install()).
struct AnalysisResult {
    Spectrogram spec;
    std::vector<unsigned char> val; // renderValues() output, one byte per cell
    unsigned int lut[256] = {};
    int valVersion = 0;
    Contour contour;
    std::vector<Segment>
        segments; // stable-pitch stretches of the contour (snapping; suggestions if chosen)
    std::vector<Segment>
        estimated; // notes from the estimator (estimate.h), the default suggestions
    std::vector<int> estimatedVoice; // voice of each, -1 if crowded out (assignVoices)
    std::vector<int> onsets;         // frames
    std::vector<double> snapTimes;   // onsets and segment edges, project time, sorted
    std::string estimateNote;        // why the estimator ran without basic-pitch, if it did
    std::string error;               // why there is no analysis (failed, or the item is too long)
};
inline AnalysisResult g_analysis;

// The window and what's on screen.
struct WindowState {
    HWND hwnd = NULL;
    ViewMap view;
    bool panel = true;
    bool showOthers = true; // show the other voices' notes, dimmed
    int voicePick = 0;      // panel buttons; applied through selectVoice()
    bool dockPending =
        false;        // toggle docking on the next timer tick, outside the window's own handlers
    HWND edit = NULL; // lyric edit field while typing; identifies its note by start and pitch
    bool analysisDirty = false; // an analysis slider moved; recomputed on mouse up
    int mouseX = -1, mouseY = -1;
};
inline WindowState g_ui;

struct Rect {
    int l, t, r, b;
    bool in(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
};

// window.cpp
Rect clientRect();
bool panelShown();
Rect noteArea();
Rect rulerArea();
Rect chordsArea();
Rect lyricsArea();
Rect overviewArea();
Rect panelArea();
void syncViewOrigin();
void redraw();
void selectVoice(int v);
int mods();
