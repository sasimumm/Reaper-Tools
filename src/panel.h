// The settings panel: widgets, presets, playback speed and the audio/notes balance.
#pragma once

#include "draw.h"
#include "state.h"

void scrollPanel(int dy);
void swapListen();
void panelMouse(int x, int y, bool down);
void drawPanel(Canvas &cv, HDC dc);
void finishPanelDrag();
