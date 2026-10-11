// Typing and pasting lyrics.
#pragma once

#include "state.h"

void placeEdit();
void beginLyric(int i);
void endLyric(bool commit, int dir);
// Before the window goes away: commit the lyric being typed, if its notes can still be written.
void finishLyric();
std::string clipboardText();
void pasteLyrics(const std::string &text);
