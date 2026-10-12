// The window's colours.
#pragma once

#include "state.h"

using Colour = unsigned int; // 0xRRGGBB

// Every colour the window draws with. The defaults are the built-in dark palette; applyTheme
// replaces the ones marked "theme" with REAPER's theme colours (by their reaper.ini keys).
struct Theme {
    // Window
    Colour bg = 0x1b1b1b;        // theme col_main_bg2: behind everything
    Colour panel = 0x222222;     // theme col_main_bg: toolbar, panel, status line
    Colour ruler = 0x262626;     // theme col_tl_bg: ruler and lanes
    Colour text = 0xd0d0d0;      // theme col_main_text2: labels and values
    Colour dim = 0;              // mixed from text and panel: secondary text
    Colour button = 0;           // mixed from panel and text: buttons and segments
    Colour track = 0;            // mixed from panel and text: hovered buttons, slider tracks
    Colour on = 0xa9b4ff;        // theme col_toolbar_text_on: toggles that are on, chosen segments
    Colour onText = 0x14161c;    // text on a bright fill
    Colour accent = 0xa9b4ff;    // UI state (hover, progress, slider fill), never a voice's colour
    Colour dirty = 0xc0a040;     // an analysis slider not applied yet
    Colour separator = 0x000000; // the 1-pixel lines between the window's parts
    Colour scrim = 0x0c0d11;     // behind the shortcuts overlay

    // Note area
    Colour area = 0x1b1b1b; // under the spectrogram, dark whatever the theme
    // Voices in the Okabe-Ito palette (colour-blind safe), readable over the graphite spectrogram.
    Colour voices[WINDOW_VOICES] = {0xe69f00, 0x56b4e9, 0xcc79a7, 0x009e73};
    Colour outline = 0xf4f6ff;    // selected notes
    Colour crowded = 0x8b91a3;    // suggestions with no voice left
    Colour contour = 0xf0e442;    // the sung pitch; yellow, never white like the play cursor
    Colour grid = 0xffffff;       // pitch rows and the tempo grid
    Colour highlight = 0xffffff;  // marquee, time selection, lyrics, the overview's view frame
    Colour shadow = 0x000000;     // outlines, backdrops, darkened notes and black-key rows
    Colour warn = 0xffa040;       // octave check flags, estimator notes, onsets
    Colour editCursor = 0xff6b5b; // the edit cursor
    Colour playCursor = 0xffffff; // the play cursor

    // Ruler and lanes
    Colour tick = 0xa0a0a0;      // ruler beats, chord boundaries, octave check marks not flagged
    Colour tickMinor = 0x707070; // ruler seconds

    // Keyboard column
    Colour keyWhite = 0xd8d8d8;
    Colour keyBlack = 0x303030;
    Colour keyEdge = 0x888888; // the line between keys
    Colour keyText = 0x202020; // octave names
};
inline Theme g_theme;

// Rebuilds g_theme: the built-in palette, with REAPER's theme colours when "Colours from REAPER
// theme" is on.
void applyTheme();

// a towards b by f (0..1).
Colour mix(Colour a, Colour b, double f);
