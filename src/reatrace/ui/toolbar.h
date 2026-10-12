// The toolbar: what you change while transcribing (voice, snap, suggestions, guides, what you hear),
// plus the shortcut sheet and the settings panel.
#pragma once

#include "ui/draw.h"

// Draws the toolbar in L.toolbar, with the item under the mouse hovered.
void drawToolbar(Canvas &cv, HDC dc, const Layout &L);

// A click at window (x, y) points; mods are MOD_* flags (shift-click hides or shows a voice).
void toolbarClick(int x, int y, int mods, const Layout &L);

std::string toolbarTip(int x, int y, const Layout &L); // for the status line
