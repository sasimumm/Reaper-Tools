// Editing commands: snapping, quantise, suggestions, clipboard, split, loop.
#pragma once

#include "state.h"

double gridStepQN();
void quantiseSelected();
const std::vector<Segment> &suggestions();
int suggestionVoice(size_t i);
bool covered(const Segment &sg);
void segmentRect(const Segment &sg, int *x0, int *y0, int *x1, int *y1);
int suggestionAt(int x, int y);
int firstSelected();
void setupEditorSnap();
void acceptSuggestion(int i);
void acceptVisibleSuggestions();
void pitchFromContour(Note &nt);
void copyNotes(bool cut);
void paste();
void setSelection(double a, double b);
void loopPlay();
void splitNotes();
