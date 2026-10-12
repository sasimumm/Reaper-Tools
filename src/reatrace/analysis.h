// Background analysis of the open item: spectrogram, contour and note estimates, cached per channel.
#pragma once

#include "state.h"

void updateValues();
void updateColormap(); // the colour curve or map changed: only the 256 colours
double frameTime(double f);
void clearCache();
void clearAnalysis(); // before analysing another item
// Which recording an audio take plays: the take, its source and the source's file. Switching takes
// or replacing the source changes it, even when the timing stays the same.
std::string audioIdentity(MediaItem_Take *take);
void cancelJob();
void requestAnalysis(bool autoCalibrateAfter);
void pollJob();
int analysisProgress();
