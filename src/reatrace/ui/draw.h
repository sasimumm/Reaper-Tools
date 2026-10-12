// Drawing into the window's backbuffer (paint.cpp and panel.cpp).
#pragma once

#include "state.h"
#include "ui/theme.h"

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
        if (x0 >= x1) {
            return;
        }
        if (a >= 256) {
            for (int y = y0; y < y1; y++) {
                std::fill(px + (size_t)y * w + x0, px + (size_t)y * w + x1, opaque(col));
            }
            return;
        }
        for (int y = y0; y < y1; y++) {
            unsigned int *p = px + (size_t)y * w;
            for (int x = x0; x < x1; x++) {
                p[x] = blend(p[x], col, a);
            }
        }
    }
    // Rectangles with their corners cut by a point: notes and suggestions.
    void rounded(int x0, int y0, int x1, int y1, unsigned int col, int a = 256) {
        if (x1 - x0 < 3 || y1 - y0 < 3) {
            fill(x0, y0, x1, y1, col, a);
            return;
        }
        fill(x0 + 1, y0, x1 - 1, y0 + 1, col, a);
        fill(x0, y0 + 1, x1, y1 - 1, col, a);
        fill(x0 + 1, y1 - 1, x1 - 1, y1, col, a);
    }
    void roundedFrame(int x0, int y0, int x1, int y1, unsigned int col, int t = 1, int a = 256) {
        fill(x0 + 1, y0, x1 - 1, y0 + t, col, a), fill(x0 + 1, y1 - t, x1 - 1, y1, col, a);
        fill(x0, y0 + 1, x0 + t, y1 - 1, col, a), fill(x1 - t, y0 + 1, x1, y1 - 1, col, a);
    }
    void frame(int x0, int y0, int x1, int y1, unsigned int col, int t = 1) {
        fill(x0, y0, x1, y0 + t, col), fill(x0, y1 - t, x1, y1, col);
        fill(x0, y0, x0 + t, y1, col), fill(x1 - t, y0, x1, y1, col);
    }
};
void text(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col, int flags);
