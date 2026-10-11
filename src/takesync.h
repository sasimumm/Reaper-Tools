// The active voice's notes <-> its MIDI take, through notesync.h.
#pragma once

#include "state.h"

MediaItem_Take *midiTake();
std::string takeHash(MediaItem_Take *take);
void loadNotes();
void loadOtherVoices(bool force);
void commitNotes();
void insertIntoVoice(int v, const std::vector<Note> &notes);
