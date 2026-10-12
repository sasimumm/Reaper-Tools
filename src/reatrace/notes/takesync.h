// The active voice's notes <-> its MIDI take, through notesync.h.
#pragma once

#include "state.h"

MediaItem_Take *midiTake(); // the active voice's notes take, NULL if it has none
bool notesStale(); // the active take changed since its notes were loaded (edits, item move, tempo)
void loadNotes();  // the active take's notes into the editor
void loadOtherVoices(bool force); // the other voices' notes, for display; force: even if unchanged
// Writes the editor's notes to the take as one undo point, then reloads them.
void commitNotes();
// Adds notes to voice v's take, creating its track and item if needed.
void insertIntoVoice(int v, const std::vector<Note> &notes);
