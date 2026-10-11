// Internals of the ReaTrace window, shared by its source files (window.cpp, paint.cpp, panel.cpp,
// analysis.cpp, takesync.cpp, settings.cpp, preview.cpp, lyrics.cpp, commands.cpp).
#pragma once

#include "window.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "reaper_plugin_functions.h"
#undef min
#undef max

#include "chords.h"
#include "contour.h"
#include "editor.h"
#include "estimate.h"
#include "guard.h"
#include "nsgt.h"

// Layout, in pixels.
static const int KB_W = 56, RULER_H = 30, STATUS_H = 20, PANEL_W = 250, LYRICS_H = 18,
                 OVERVIEW_H = 28, CHORDS_H = 20;
// Colours that follow REAPER's theme (paint.cpp's applyTheme), 0xRRGGBB.
struct Theme {
    unsigned int bg = 0, ruler = 0, text = 0, dim = 0, panel = 0, button = 0, track = 0;
};
inline Theme g_theme;
static const unsigned int C_NOTE = 0x2ec4b6, C_NOTE_EDGE = 0x08302d, C_SEL = 0xeafffd;
static const unsigned int C_EDIT_CURSOR = 0xff5050, C_PLAY_CURSOR = 0xffffff, C_ACCENT = 0x7fd6ff;

// --- state ---------------------------------------------------------------------

struct OtherVoice {
    MediaItem *item = NULL;
    std::string hash;
    std::vector<Note> notes;
};
// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };

const std::vector<Segment> &suggestions();
int suggestionVoice(size_t i);

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
void updateValues();
double frameTime(double f);
void clearCache();
void clearAnalysis(); // before analysing another item
void cancelJob();
void requestAnalysis(bool autoCalibrateAfter);
void pollJob();
int analysisProgress();
double overviewTime(int x);
MediaItem_Take *midiTake();
std::string takeHash(MediaItem_Take *take);
void loadNotes();
void loadOtherVoices(bool force);
void commitNotes();
double gridStepQN();
void quantiseSelected();

// --- drawing helpers -------------------------------------------------------------

// Coordinates and clip are in points; px/w/h are the device-pixel backbuffer.
struct Canvas {
    unsigned int *px;
    int w, h;
    Rect clip;
    int scale; // device pixels per point

    void fill(int x0, int y0, int x1, int y1, unsigned int col, int a = 256) {
        const int s = scale;
        x0 = std::max(std::max(x0, clip.l) * s, 0), y0 = std::max(std::max(y0, clip.t) * s, 0);
        x1 = std::min(std::min(x1, clip.r) * s, w), y1 = std::min(std::min(y1, clip.b) * s, h);
        for (int y = y0; y < y1; y++) {
            unsigned int *p = px + (size_t)y * w;
            for (int x = x0; x < x1; x++) {
                if (a >= 256) {
                    p[x] = 0xff000000 | col;
                    continue;
                }
                const unsigned int d = p[x];
                const unsigned int r =
                    (((d >> 16) & 255) * (256 - a) + ((col >> 16) & 255) * a) >> 8;
                const unsigned int g = (((d >> 8) & 255) * (256 - a) + ((col >> 8) & 255) * a) >> 8;
                const unsigned int b = ((d & 255) * (256 - a) + (col & 255) * a) >> 8;
                p[x] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
    }
    void frame(int x0, int y0, int x1, int y1, unsigned int col, int t = 1) {
        fill(x0, y0, x1, y0 + t, col), fill(x0, y1 - t, x1, y1, col);
        fill(x0, y0, x0 + t, y1, col), fill(x1 - t, y0, x1, y1, col);
    }
};
void text(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col, int flags);
void scrollPanel(int dy);

static const char *EXT_SECTION = "reatrace", *EXT_DEFAULTS = "defaults";
const char *globalState(const char *key);
std::string saveSettings();
void loadSettings(const char *str);
void resetSettings();
bool loadItemSettings();
void saveItemSettings();
void swapListen();
void panelMouse(int x, int y, bool down);
void drawPanel(Canvas &cv, HDC dc);
bool covered(const Segment &sg);
void segmentRect(const Segment &sg, int *x0, int *y0, int *x1, int *y1);
int suggestionAt(int x, int y);
int lyricAt(int x);
void paint(HDC hdc);
void releasePaintCache(); // the window is going away
void stopPreviews(bool onlyFinished);
void previewPitch(int pitch);

void previewDragged();
void previewDragStarted(int pitch); // a drag starts on a note already heard at this pitch
void placeEdit();
void beginLyric(int i);
void endLyric(bool commit, int dir);
void selectVoice(int v);
int firstSelected();
int mods();
void setupEditorSnap();
void acceptSuggestion(int i);
void insertIntoVoice(int v, const std::vector<Note> &notes);
void acceptVisibleSuggestions();
void pitchFromContour(Note &nt);
std::string clipboardText();
void copyNotes(bool cut);
void paste();
void setSelection(double a, double b);
void loopPlay();
void splitNotes();
void pasteLyrics(const std::string &text);
void finishPanelDrag();
