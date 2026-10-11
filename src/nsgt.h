// Constant-Q-style spectrogram (NSGT with Gaussian frequency windows), ported from DrawScore.
#pragma once

#include <functional>
#include <vector>

struct NSGTParams
{
  double sampleRate = 22050; // analysis rate; audio is resampled to this
  double fmin = 27.5;        // A0, bin 0
  double fmax = 8000;
  int binsPerOctave = 60;    // 5 bins per semitone
  int hop = 256;             // samples per frame, power of two
  double gamma = 11;         // Hz added to every bin's bandwidth: lower = sharper pitch, blurrier timing
};

struct Spectrogram
{
  NSGTParams p;
  int bins = 0, frames = 0;
  std::vector<float> mag;   // frames*bins, linear amplitude, a full-scale sine is ~1
  std::vector<float> noise; // per bin: mean of the quietest 10% of frames

  float at(int f, int b) const { return mag[(size_t)f * bins + b]; }
  double frameRate() const { return p.sampleRate / p.hop; }
  double binOfPitch(double midiPitch) const; // continuous bin index for a (fractional) MIDI pitch
};

// read(start, n, out) fills out[0..n) with mono samples from sample `start`; out-of-range samples are 0.
Spectrogram computeNSGT(const NSGTParams &p, long long numSamples,
                        const std::function<void(long long start, int n, float *out)> &read);

struct RenderParams
{
  double dbFloor = -80, brightness = 0, contrast = 1;
  double bias = -0.8;         // colormap gamma: t^(2^-bias), negative darkens the low end
  bool subtractNoise = false;
  double noiseAlpha = 1.5;
  bool peaksOnly = false;     // dim everything except per-frame local maxima along frequency
};

// Picks dbFloor/contrast from the 30th and 95th percentile of the data (from Specter).
void autoCalibrate(const Spectrogram &s, RenderParams &rp);

// Display value in [0,1] for one cell.
double cellValue(const Spectrogram &s, const RenderParams &rp, int frame, int bin);

// 256-entry magma colormap with the bias curve applied, as 0x00RRGGBB.
void makeColormap(double bias, unsigned int lut[256]);
