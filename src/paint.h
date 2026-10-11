// Drawing the window.
#pragma once

#include "state.h"

double overviewTime(int x);
int lyricAt(int x);
unsigned laneLabelAt(int x, int y, const Layout &L); // LANE_* whose label is there, or 0
void paint(HDC hdc);
void releasePaintCache(); // the window is going away
