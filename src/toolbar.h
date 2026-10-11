// The toolbar: what you change while transcribing (voice, snap, suggestions, guides, what you hear),
// plus the shortcut sheet and the settings panel.
#pragma once

#include "draw.h"

void drawToolbar(Canvas &cv, HDC dc, const Layout &L);
void toolbarClick(int x, int y, int mods, const Layout &L);
std::string toolbarTip(int x, int y, const Layout &L); // for the status line
