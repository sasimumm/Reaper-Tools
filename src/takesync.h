// The active voice's notes <-> its MIDI take, through notesync.h.
#pragma once

#include "state.h"

MediaItem_Take *midiTake();
bool notesStale(); // the active take changed since its notes were loaded (edits, item move, tempo)
void loadNotes();
void loadOtherVoices(bool force);
void commitNotes();
void insertIntoVoice(int v, const std::vector<Note> &notes);
