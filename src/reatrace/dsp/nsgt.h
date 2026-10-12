// Constant-Q-style spectrogram (NSGT with Gaussian frequency windows), ported from DrawScore.
#pragma once

#include "index.h"
#include <functional>
#include <vector>

struct NSGTParams {
    double sampleRate = 22050; // analysis rate; audio is resampled to this
    double fmin = 27.5;        // A0, bin 0
    double fmax = 8000;
    int binsPerOctave = 60; // 5 bins per semitone
    int hop = 256;          // samples per frame, power of two
    double gamma = 11; // Hz added to every bin's bandwidth: lower = sharper pitch, blurrier timing
};

struct Spectrogram {
    NSGTParams p;
    int bins = 0, frames = 0;
    std::vector<float> mag;   // frames*bins, linear amplitude, a full-scale sine is ~1
    std::vector<float> noise; // per bin: mean spectrum of the quietest frames, see estimateNoise()

    float at(int f, int b) const { return mag[idx(f, bins, b)]; }
    double frameRate() const { return p.sampleRate / p.hop; }
    double binOfPitch(double midiPitch) const; // continuous bin index for a (fractional) MIDI pitch
};

// read(start, n, out) fills out[0..n) with mono samples from sample `start`; out-of-range samples are 0.
// progress(fraction) is called after each block; returning false stops early (result incomplete).
Spectrogram computeNSGT(const NSGTParams &p, long long numSamples,
                        const std::function<void(long long start, int n, float *out)> &read,
                        const std::function<bool(double)> &progress = nullptr);

// Noise floor for spectral subtraction: mean spectrum of the quietest `fraction` of frames.
void estimateNoise(Spectrogram &s, double fraction);

struct RenderParams {
    double dbFloor = -80, brightness = 0, contrast = 1;
    double bias = -0.8; // colormap gamma: t^(2^-bias), negative darkens the low end
    bool subtractNoise = false;
    double noiseAlpha = 1.5;
    double noiseFraction = 0.1; // passed to estimateNoise()
    bool peaksOnly = false;     // dim everything except per-frame local maxima along frequency
    double peakDim = 0.15;      // brightness kept by non-peaks
    int colormap = 0;           // COLORMAP_*
};

enum { COLORMAP_GRAPHITE, COLORMAP_MAGMA };

// Picks dbFloor/contrast from the 30th and 95th percentile of frames [f0, f1) (from Specter).
void autoCalibrate(const Spectrogram &s, RenderParams &rp, int f0 = 0, int f1 = -1);

// Display value in [0,1] for one cell.
double cellValue(const Spectrogram &s, const RenderParams &rp, int frame, int bin);

// cellValue()*255 for every cell, fast enough to run while dragging a slider.
void renderValues(const Spectrogram &s, const RenderParams &rp, std::vector<unsigned char> &out);

// 256-entry magma colormap with the bias curve applied, as 0x00RRGGBB.
void makeColormap(double bias, unsigned int lut[256], int colormap = COLORMAP_GRAPHITE);
