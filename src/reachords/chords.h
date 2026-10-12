// Chord items: a media source holding a chord progression, drawn and edited inside
// its item in the arrange view.
#pragma once

#include <string>
#include <vector>

#include "reaper_plugin.h"

// Registers the chord item source type and the "ReaChords: Insert chord item" action.
void Chords_Register(reaper_plugin_info_t *rec);
bool Chords_OnAction(int command); // true if it was the "insert chord item" action
// Keys while typing a chord, as an accelerator hook: 0 if not ours, 1 if handled, -1 to let the
// edit field have it.
int Chords_TranslateAccel(MSG *msg);
void Chords_Timer(); // on REAPER's timer: keeps the chord edit field focused, closes it on blur

// A chord as it sounds in the project: clipped to its item, in project seconds.
struct ChordSpan {
    double start, end;
    std::string name;
};
// All chord items' chords, sorted by start. Rebuilt when the project changes; the reference stays
// valid until the next call.
const std::vector<ChordSpan> &Chords_Spans();
