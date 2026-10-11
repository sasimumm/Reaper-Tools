// Drawing into the window's backbuffer, and the colours (paint.cpp and panel.cpp).
#pragma once

#include "state.h"

// Colours that follow REAPER's theme (paint.cpp's applyTheme), 0xRRGGBB.
struct Theme {
    unsigned int bg = 0, ruler = 0, text = 0, dim = 0, panel = 0, button = 0, track = 0;
};
inline Theme g_theme;
static const unsigned int C_NOTE = 0x2ec4b6, C_NOTE_EDGE = 0x08302d, C_SEL = 0xeafffd;
static const unsigned int C_EDIT_CURSOR = 0xff5050, C_PLAY_CURSOR = 0xffffff, C_ACCENT = 0x7fd6ff;

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
