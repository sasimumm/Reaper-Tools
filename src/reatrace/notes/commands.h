// Editing commands: snapping, quantise, suggestions, clipboard, split, loop.
#pragma once

#include "state.h"

double gridStepQN(); // grid spacing in quarter notes for the current zoom, 0 for measures
void quantiseSelected();
void nudgeSelected(int steps, int semitones); // grid steps, semitones

// Suggested notes (indices into suggestions()) and the voice each would go to.
const std::vector<Segment> &suggestions();
int suggestionVoice(size_t i);
// its voice is hidden in the toolbar: not drawn, hovered or accepted
bool suggestionHidden(size_t i);
bool covered(size_t i); // suggestion i is covered by a drawn note
void segmentRect(const Segment &sg, int *x0, int *y0, int *x1, int *y1); // window pixels
int suggestionAt(int x, int y); // index of the suggestion at a window pixel, -1 if none

int firstSelected();    // earliest selected note's index, -1 if none
void setupEditorSnap(); // sets the editor's snap from the snap mode setting
void acceptSuggestion(int i);
void acceptVisibleSuggestions();
void copyNotes(bool cut);
void moveSelectedToVoice(int v); // cmd+1..4
// The key: the edited voice's take scale (see MIDI_GetScale), else the project's key signature as
// last saved; its root, scale and name. False without either.
bool currentKey(int *root, int *scale, std::string *name);
// Shift+I: a field under the earliest selected note for intervals to add (see parseIntervals).
// Enter adds them (commit; the field stays open on a typo), Esc closes it.
void beginIntervals();
void endIntervals(bool commit);
void paste();
void setSelection(double a, double b); // time selection and loop points, seconds
void loopPlay();
void splitNotes();
