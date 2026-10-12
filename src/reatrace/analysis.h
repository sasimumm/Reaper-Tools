// Background analysis of the open item: spectrogram, contour and note estimates, cached per
// channel.
#pragma once

#include "state.h"

void updateValues();        // the display settings changed: the spectrogram's values and colours
void updateColormap();      // the colour curve or map changed: only the 256 colours
double frameTime(double f); // spectrogram frame -> project time, seconds
void clearCache();          // drops every cached spectrogram and estimate
void clearAnalysis();       // before analysing another item
// Which recording an audio take plays: the take, its source and the source's file. Switching takes
// or replacing the source changes it, even when the timing stays the same.
std::string audioIdentity(MediaItem_Take *take);
void cancelJob(); // stops and waits for the running analysis, if any
// Starts the analysis for the current settings in the background (or installs it from the cache);
// autoCalibrateAfter: fit the display settings to it once it arrives.
void requestAnalysis(bool autoCalibrateAfter);
void pollJob();         // from the timer: installs a finished analysis
int analysisProgress(); // permille, -1 if none is running
