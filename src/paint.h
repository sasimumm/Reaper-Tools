// Drawing the window.
#pragma once

#include "state.h"

double overviewTime(int x);
int lyricAt(int x);
void paint(HDC hdc);
void releasePaintCache(); // the window is going away
