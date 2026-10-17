// The tracks panel: the project's tracks with mute and solo buttons, in the side panel's place.
#pragma once

#include "state.h"
#include "ui/draw.h"

// A mouse press (down) or drag in the panel at client point x, y: pressing a mute or solo button
// flips it, and dragging up or down sets the others passed over the same way.
void tracksMouse(int x, int y, bool down);
void finishTracksDrag();
void scrollTracks(int dy); // by dy points, kept within the list
void drawTracks(Canvas &cv, HDC dc);
