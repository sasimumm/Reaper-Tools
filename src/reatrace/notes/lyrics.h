// Typing and pasting lyrics.
#pragma once

#include "state.h"

void placeEdit();       // keeps the edit field over its note after the view moved
void beginLyric(int i); // opens the edit field on note i
// Closes the edit field, storing the text if `commit`; then edits the next (dir 1) or previous
// (dir -1) note, or none (0).
void endLyric(bool commit, int dir);
// Before the window goes away: commit the lyric being typed, if its notes can still be written.
void finishLyric();
std::string clipboardText();
// One syllable per note (see splitSyllables), to the selected notes or from the selected one on.
void pasteLyrics(const std::string &text);
