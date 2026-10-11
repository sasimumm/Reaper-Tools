// Typing and pasting lyrics.
#pragma once

#include "state.h"

void placeEdit();
void beginLyric(int i);
void endLyric(bool commit, int dir);
std::string clipboardText();
void pasteLyrics(const std::string &text);
