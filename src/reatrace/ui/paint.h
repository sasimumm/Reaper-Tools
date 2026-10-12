// Drawing the window, and the hit tests that depend on what was drawn.
#pragma once

#include "state.h"

// The time (seconds) at window x (points) in the overview lane.
double overviewTime(int x);

// The note whose syllable is shown at window x (points) in the lyrics lane, -1 if none.
int lyricAt(int x);

unsigned laneLabelAt(int x, int y, const Layout &L); // LANE_* whose label is there, or 0

// Paints the whole window into a backbuffer and blits it to hdc.
void paint(HDC hdc);

void releasePaintCache(); // the window is going away
