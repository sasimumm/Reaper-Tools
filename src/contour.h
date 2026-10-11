// Vocal analysis on top of the spectrogram: pitch contour, stable-pitch segments and onsets.
#pragma once

#include <vector>

#include "nsgt.h"

struct Contour {
    std::vector<float> pitch;    // per frame, continuous MIDI pitch; < 0 where unvoiced
    std::vector<float> strength; // per frame, harmonic score of the chosen pitch (0..1)
};

// Harmonic-sum pitch tracking over the vocal range, with a Viterbi pass across candidates that
// penalises jumps, so formant-boosted overtones don't cause octave errors.
Contour trackPitch(const Spectrogram &s, double lowPitch = 36, double highPitch = 88);

// A stretch where the (vibrato-smoothed) pitch stays within `tolerance` semitones.
struct Segment {
    int f0, f1;   // frames, inclusive
    double pitch; // median pitch, skipping the first `skipStart` seconds (scoops)
};

// Boundaries are refined: between two notes, to where the raw pitch crosses halfway; a start,
// to an onset within 60 ms when `onsets` is given (the analysis smears note edges by ~50 ms).
std::vector<Segment> segmentContour(const Contour &c, double frameRate,
                                    const std::vector<int> *onsets = nullptr,
                                    double minDuration = 0.08, double tolerance = 0.5,
                                    double skipStart = 0.05);

// Frames where new sound starts: peaks of positive spectral flux above mean + 1.5 std (from Specter).
std::vector<int> detectOnsets(const Spectrogram &s);

// Median of the voiced contour between frames f0..f1 (inclusive), skipping the first `skip`
// frames when there are enough; returns false if fewer than half the frames are voiced.
bool contourMedian(const Contour &c, int f0, int f1, int skip, double *pitch);
