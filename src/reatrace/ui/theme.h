// The window's colours, 0xRRGGBB: the ones that follow REAPER's theme, and the fixed ones.
#pragma once

#include "state.h"

// Colours that follow REAPER's theme (applyTheme).
struct Theme {
    unsigned int bg = 0, ruler = 0, text = 0, dim = 0, panel = 0, button = 0, track = 0;
    unsigned int on = 0; // toolbar toggles that are on and chosen segments
};
inline Theme g_theme;

// Reads the theme colours by their reaper.ini keys, or the built-in dark colours without
// "Colours from REAPER theme".
void applyTheme();

// a towards b by f (0..1).
unsigned int mix(unsigned int a, unsigned int b, double f);

// C_ACCENT marks UI state (hover, progress, slider fill) and is never a voice's colour.
static const unsigned int C_EDIT_CURSOR = 0xff6b5b, C_PLAY_CURSOR = 0xffffff, C_ACCENT = 0xa9b4ff;
// Voices in the Okabe-Ito palette (colour-blind safe), readable over the graphite spectrogram.
static const unsigned int VOICE_COLOURS[WINDOW_VOICES] = {0xe69f00, 0x56b4e9, 0xcc79a7, 0x009e73};
// The note area stays dark under the spectrogram, whatever the theme.
static const unsigned int C_AREA = 0x1b1b1b;
static const unsigned int C_CONTOUR = 0xf0e442; // yellow: never white like the play cursor
static const unsigned int C_OUTLINE = 0xf4f6ff; // selected notes
static const unsigned int C_CROWDED = 0x8b91a3; // suggestions with no voice left
static const unsigned int C_WARN = 0xffa040;    // octave check flags, estimator notes, onsets
static const unsigned int C_DIRTY = 0xc0a040;   // an analysis slider not applied yet
static const unsigned int C_ON_TEXT = 0x14161c; // text on a bright fill
// The keyboard column.
static const unsigned int C_KEY_WHITE = 0xd8d8d8, C_KEY_BLACK = 0x303030, C_KEY_EDGE = 0x888888,
                          C_KEY_TEXT = 0x202020;
