// The window's colours. RGB is a type here; SWELL's RGB(r, g, b) macro only expands before "(".
#pragma once

#include "state.h"

using RGB = unsigned int;  // 0xRRGGBB
using ARGB = unsigned int; // 0xAARRGGBB: a backbuffer or layer pixel, always opaque

inline ARGB opaque(RGB c) { return 0xff000000 | c; }

// col over the pixel d with alpha a (0..256). Red and blue are blended together, green apart:
// two multiplies per channel pair.
inline ARGB blend(ARGB d, RGB col, int a) {
    const unsigned int ia = static_cast<unsigned int>(256 - a), ua = static_cast<unsigned int>(a);
    return opaque(((((d & 0xff00ff) * ia + (col & 0xff00ff) * ua) >> 8) & 0xff00ff) |
                  ((((d & 0xff00) * ia + (col & 0xff00) * ua) >> 8) & 0xff00));
}

// Every colour the window draws with. The defaults are the built-in dark palette; applyTheme
// replaces the ones marked "theme" with REAPER's theme colours (by their reaper.ini keys).
struct Theme {
    // Window
    RGB bg = 0x1b1b1b;        // theme col_main_bg2: behind everything
    RGB panel = 0x222222;     // theme col_main_bg: toolbar, panel, status line
    RGB ruler = 0x262626;     // theme col_tl_bg: ruler and lanes
    RGB text = 0xd0d0d0;      // theme col_main_text2: labels and values
    RGB dim = 0;              // mixed from text and panel: secondary text
    RGB button = 0;           // mixed from panel and text: buttons and segments
    RGB track = 0;            // mixed from panel and text: hovered buttons, slider tracks
    RGB on = 0xa9b4ff;        // theme col_toolbar_text_on: toggles that are on, chosen segments
    RGB onText = 0x14161c;    // text on a bright fill
    RGB accent = 0xa9b4ff;    // UI state (hover, progress, slider fill), never a voice's colour
    RGB dirty = 0xc0a040;     // an analysis slider not applied yet
    RGB separator = 0x000000; // the 1-pixel lines between the window's parts
    RGB scrim = 0x0c0d11;     // behind the shortcuts overlay

    // Note area
    RGB area = 0x1b1b1b; // under the spectrogram, dark whatever the theme
    // Voices in the Okabe-Ito palette (colour-blind safe), readable over the graphite spectrogram.
    RGB voices[WINDOW_VOICES] = {0xe69f00, 0x56b4e9, 0xcc79a7, 0x009e73};
    RGB outline = 0xf4f6ff;    // selected notes
    RGB crowded = 0x8b91a3;    // suggestions with no voice left
    RGB contour = 0xf0e442;    // the sung pitch; yellow, never white like the play cursor
    RGB grid = 0xffffff;       // pitch rows and the tempo grid
    RGB highlight = 0xffffff;  // marquee, time selection, lyrics, the overview's view frame
    RGB shadow = 0x000000;     // outlines, backdrops, darkened notes and black-key rows
    int blackRowShade = 40;    // how much shadow covers the black-key rows (of 256)
    RGB warn = 0xffa040;       // octave check flags, estimator notes, onsets
    RGB editCursor = 0xff6b5b; // the edit cursor
    RGB playCursor = 0xffffff; // the play cursor

    // Ruler and lanes
    RGB tick = 0xa0a0a0;      // ruler beats, chord boundaries, octave check marks not flagged
    RGB tickMinor = 0x707070; // ruler seconds

    // Keyboard column
    RGB keyWhite = 0xd8d8d8;
    RGB keyBlack = 0x303030;
    RGB keyEdge = 0x888888; // the line between keys
    RGB keyText = 0x202020; // octave names

    // A note area pixel in a black key's row.
    ARGB shadeBlack(ARGB px) const { return blend(px, shadow, blackRowShade); }
};
inline Theme g_theme;

// Rebuilds g_theme: the built-in palette, with REAPER's theme colours when "Colours from REAPER
// theme" is on.
void applyTheme();

// a towards b by f (0..1).
RGB mix(RGB a, RGB b, double f);
