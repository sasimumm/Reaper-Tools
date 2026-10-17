// The side panel: the settings (S) about the spectrogram and the guides, or the tracks (M) with
// the analysis, playback and window sections; widgets, presets, playback speed and the audio/notes
// balance.
#pragma once

#include "state.h"
#include "ui/draw.h"

void scrollPanel(int dy); // by dy points, kept within the content
void swapListen();        // A/B: hear only the audio, or only the notes
// What is heard (0 both, 1 audio only, 2 notes only) and the speed (0 100%, 1 75%, 2 50%), as
// REAPER has them after syncPlayback(); the toolbar shows and changes them.
void syncPlayback();
int listenMode();
void setListen(int mode);
int speedIndex();
void setSpeed(int index);
// A mouse press (down) or drag in the panel at client point x, y.
void panelMouse(int x, int y, bool down);
void panelDoubleClick(int x, int y); // resets a slider to its default
void drawPanel(Canvas &cv, HDC dc);
void finishPanelDrag(); // mouse up: recomputes the analysis if an analysis slider moved
