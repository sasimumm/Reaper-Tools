// The tracks on the tracks panel (M): the project's tracks with mute and solo buttons, drawn and
// handled by the side panel above the sections it shows there.
#pragma once

#include "state.h"
#include "ui/draw.h"

int tracksHeight(); // of the rows and the space below them, in points
void drawTrackRows(Canvas &cv, HDC dc, int top);
// A mouse press (down) or drag at client point x, y, the rows starting at top: pressing a mute or
// solo button flips it, and dragging up or down sets the others passed over the same way. True if
// the tracks took it.
bool tracksMouse(int x, int y, int top, bool down);
void finishTracksDrag();
