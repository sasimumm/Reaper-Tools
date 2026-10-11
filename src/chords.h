// Chord items: a media source holding a chord progression, drawn and edited inside
// its item in the arrange view.
#pragma once

#include <string>
#include <vector>

#include "reaper_plugin.h"

void Chords_Register(reaper_plugin_info_t *rec);
bool Chords_OnAction(int command);   // true if it was the "insert chord item" action
int Chords_TranslateAccel(MSG *msg); // keys while typing a chord; 0 if not ours
void Chords_Timer();

// A chord as it sounds in the project: clipped to its item, in project seconds.
struct ChordSpan {
    double start, end;
    std::string name;
};
const std::vector<ChordSpan> &Chords_Spans(); // all chord items' chords, sorted by start
