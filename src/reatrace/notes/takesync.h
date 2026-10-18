// The active voice's notes <-> its MIDI take, through notesync.h.
#pragma once

#include "state.h"

#include <span>

MediaItem_Take *midiTake(); // the active voice's notes take, NULL if it has none
bool notesStale(); // the active take changed since its notes were loaded (edits, item move, tempo)
void loadNotes();  // the active take's notes into the editor
void loadOtherVoices(bool force); // the other voices' notes, for display; force: even if unchanged
// Writes the editor's notes to the take as one undo point, then reloads them; false if the take
// changed since they were loaded (the edit is dropped).
bool commitNotes();
// Adds notes to voice v's take, creating its track and item if needed.
void insertIntoVoice(int v, std::span<const Note> notes);
// Makes REAPER's selected tracks with MIDI items (and those in selected folders) the reference,
// leaving out the voice tracks; false if there are none.
bool pickReference();
// The reference tracks' notes over the audio item into the session, when suggesting from them.
void loadReference();
bool canReference(MediaTrack *tr); // has MIDI and isn't one of the item's voice tracks
bool isReference(MediaTrack *tr);
// Adds the track to the reference (last) or takes it out, suggesting from the reference while it
// has any tracks.
void setReference(MediaTrack *tr, bool on);
