// The settings panel: widgets, presets, playback speed and the audio/notes balance.
#pragma once

#include "draw.h"
#include "state.h"

void scrollPanel(int dy);
void swapListen();
// What is heard (0 both, 1 audio only, 2 notes only) and the speed (0 100%, 1 75%, 2 50%), as
// REAPER has them after syncPlayback(); the toolbar shows and changes them.
void syncPlayback();
int listenMode();
void setListen(int mode);
int speedIndex();
void setSpeed(int index);
void panelMouse(int x, int y, bool down);
void panelDoubleClick(int y); // resets a slider to its default
void drawPanel(Canvas &cv, HDC dc);
void finishPanelDrag();
