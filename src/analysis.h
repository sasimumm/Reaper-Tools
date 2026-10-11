// Background analysis of the open item: spectrogram, contour and note estimates, cached per channel.
#pragma once

#include "state.h"

void updateValues();
double frameTime(double f);
void clearCache();
void clearAnalysis(); // before analysing another item
void cancelJob();
void requestAnalysis(bool autoCalibrateAfter);
void pollJob();
int analysisProgress();
