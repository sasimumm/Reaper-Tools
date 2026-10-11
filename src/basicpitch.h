// Spotify's basic-pitch (Apache-2.0, vendor/basic-pitch) run through CoreML: note and onset
// posteriors per semitone, used to confirm the NMF note estimates (see estimate.h).
#pragma once

#include <functional>
#include <string>
#include <vector>

struct BasicPitch {
    static const int PITCHES = 88, LOWEST = 21; // A0 .. C8
    int frames = 0; // on the uniform 256-sample hop at 22050 Hz: frame f is sample f*256
    std::vector<float> note, onset; // frames*PITCHES, 0..1

    float noteAt(int f, int midi) const { return note[(size_t)f * PITCHES + midi - LOWEST]; }
    float onsetAt(int f, int midi) const { return onset[(size_t)f * PITCHES + midi - LOWEST]; }
};

// mono: audio at 22050 Hz. modelPath: the compiled model (nmp.mlmodelc). progress(fraction)
// returning false cancels. Returns false (and sets *error) if the model can't be loaded or run.
bool runBasicPitch(const std::string &modelPath, const std::vector<float> &mono, int frames,
                   BasicPitch &out, std::string *error = nullptr,
                   const std::function<bool(double)> &progress = nullptr);
