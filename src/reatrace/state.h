// The ReaTrace window's state and layout, shared by its source files; each module's functions
// are in its own header (analysis.h, paint.h, panel.h, ...).
#pragma once

#include "ui/window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "reaper_plugin_functions.h"
#undef min
#undef max

#include "dsp/contour.h"
#include "dsp/nsgt.h"
#include "model.h"
#include "notes/editor.h"
#include "notes/notesync.h"

// --- state ---------------------------------------------------------------------

// Another voice than the active one: its notes item (NULL until it has notes) and its notes as of
// `snapshot`, drawn dimmed.
struct OtherVoice {
    MediaItem *item = NULL;
    TakeSnapshot snapshot;
    std::vector<Note> notes;
};

// The open item's settings (settings.cpp loads and saves them).
inline Settings g_settings;

// The item being transcribed and the active voice's notes (window.cpp opens it, takesync.cpp
// syncs it).
struct Session {
    MediaItem *audio = NULL, *midi = NULL; // midi: the active voice's notes item
    int voice = 0;                         // active voice, 0 = lead
    std::array<OtherVoice, WINDOW_VOICES> others;
    // as analysed: item position and length, take start offset (seconds), take playrate
    double audioPos = 0, audioLen = 0, audioOffs = 0, audioRate = 1;
    std::string audioIdentity; // which recording: see audioIdentity()
    NoteEditor ed;
    TakeSnapshot snapshot; // of the take when the notes were loaded
    int notesVersion = 0;  // bumped whenever notes are loaded
};
inline Session g_session;

// The installed analysis of the item (written only by analysis.cpp's install()).
struct AnalysisResult {
    // shared with the analysis cache (no copy of ~150 MB per 15 minutes); never null
    std::shared_ptr<Spectrogram> spec = std::make_shared<Spectrogram>();
    std::vector<unsigned char> val;         // renderValues() output, one byte per cell
    std::array<unsigned int, 256> lut = {}; // value byte -> colour
    int valVersion = 0; // bumped whenever val or lut change, for the caches drawn from them
    Contour contour;
    // stable-pitch stretches of the contour (snapping; suggestions if chosen)
    std::vector<Segment> segments;
    // notes from the estimator (estimate.h), the default suggestions
    std::vector<Segment> estimated;
    std::vector<int> estimatedVoice; // voice of each, -1 if crowded out (assignVoices)
    std::vector<int> onsets;         // spectrogram frames
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
    // toggle docking on the next timer tick, outside the window's own handlers
    bool dockPending = false;
    HWND edit = NULL; // lyric edit field while typing; identifies its note by start and pitch
    bool analysisDirty = false;   // an analysis slider moved; recomputed on mouse up
    int mouseX = -1, mouseY = -1; // last mouse position in the client area, points
    bool help = false;            // the shortcut sheet is shown (?)
    unsigned hiddenVoices = 0;    // bit per voice: not drawn (shift-click its toolbar chip)
    unsigned foldedLanes = 0;     // LANE_* folded to a strip (click its label)
};
inline WindowState g_ui;

// window.cpp
Layout layout(); // of the window as it is now
// The parts of layout(), in client points.
Rect clientRect();
bool panelShown();
Rect noteArea();
Rect rulerArea();
Rect chordsArea();
Rect lyricsArea();
Rect overviewArea();
Rect panelArea();
void syncViewOrigin();   // puts the view's origin at the notes area's top left
void redraw();           // invalidates the window, if open
void selectVoice(int v); // makes voice v (0 = lead) the edited one, and shows it
int mods();              // MOD_* keys held now
