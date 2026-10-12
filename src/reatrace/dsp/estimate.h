// Polyphonic note estimation, ported from the `transcribe` project (docs/NOTE-ESTIMATION.md there):
// harmonic-template NMF on the NSGT with templates adapted to the recording, confirmed by
// basic-pitch, with notes the enhanced summary autocorrelation (SACF) finds added. Recordings are
// free tempo, so frames are segmented with hysteresis and a minimum length instead of a beat grid.
#pragma once

#include <functional>
#include <vector>

#include "dsp/basicpitch.h"
#include "dsp/nsgt.h"

// One estimated note on the spectrogram's frame axis.
struct NoteEstimate {
    int f0, f1;     // frames [f0, f1)
    int pitch;      // MIDI
    float strength; // mean activity over the note, on the p99-normalised NMF scale
};

// Tuning of estimateNotes; the defaults are the ones tested in transcribe.
struct EstimateParams {
    int lo = 40, hi = 86;          // note pitches (E2..D6)
    double abs = 0.06, rel = 0.15; // NMF activity: of the song's p99, and of the frame's maximum
    double oct = 2.0;              // an upper octave must be this much stronger than the note below
    double gate = 0.12;            // basic-pitch note posterior needed within +-1 semitone
    double sacfAbs = 0.25;         // SACF activity threshold; 0 = no SACF union
    double minLength = 0.1;        // seconds
    double bridge = 0.07;          // seconds of dropout bridged inside a note
    bool adapt = true;             // learn partial amplitudes on the recording
    // add voices an octave above a note (even-partial surplus, common-fate veto)
    bool octaveDoublings = true;
    // front-end vote: add notes that CQT60, a long-window CQT36 and CFP all find
    bool vote = true;
};

// The spectrogram the estimator expects (fixed, since the display's settings can change).
NSGTParams estimateNSGTParams();

// s: computed with estimateNSGTParams(); mono: the same channel at 22050 Hz; basicPitch: gives
// basic-pitch on mono, or null to run without the gate (more false notes). It is only called after
// the main NMF, so basic-pitch can run alongside it. progress(fraction) returning false cancels
// (the result is then empty or partial). Notes are sorted by start frame, then pitch.
std::vector<NoteEstimate> estimateNotes(const Spectrogram &s, const std::vector<float> &mono,
                                        const std::function<const BasicPitch *()> &basicPitch,
                                        const EstimateParams &ep = EstimateParams(),
                                        const std::function<bool(double)> &progress = nullptr);

// As above with basic-pitch already computed (bp may be null; it must outlive the call).
inline std::vector<NoteEstimate>
estimateNotes(const Spectrogram &s, const std::vector<float> &mono, const BasicPitch *bp,
              const EstimateParams &ep = EstimateParams(),
              const std::function<bool(double)> &progress = nullptr) {
    return estimateNotes(s, mono, [bp] { return bp; }, ep, progress);
}

// Voices for estimated notes (transcribe notes.voice_count / assign_voices): the most voices that
// sound together in >= 0.3% of sung frames (up to maxVoices), as pitch-ordered strands that never
// cross. Each new note takes the free strand that best continues its last pitch, a strand silent for
// a while costing more (keeps a solo line in one strand). The strand singing most is voice 0 (the
// lead); the others follow top to bottom. -1: crowded out (more notes than voices). frameRate is
// in frames per second; the result is parallel to notes.
std::vector<int> assignVoices(const std::vector<NoteEstimate> &notes, double frameRate,
                              int maxVoices);

// Stages, exposed for the self-check. Rolls are frames x (hi-lo+1), row-major, normalised to the p99
// of frame maxima.

// Harmonic-template NMF activity per semitone lo..hi on s (bins below C2 ignored); adapt learns
// the partial amplitudes, harmonics limits the partials per template (1 for CFP, which has none).
// Cancelled: all zeros.
std::vector<float> nmfRoll(const Spectrogram &s, int lo, int hi, bool adapt,
                           const std::function<bool(double)> &progress = nullptr,
                           int harmonics = 12);

// The vote's front ends (transcribe mf0.cqt / mf0.cfp), on a 7-octave axis from C2 at hop 256:
// librosa's CQT magnitude (a sine of amplitude A reads A/2 * sqrt(window length)), with Hann windows
// filterScale times the constant-Q length; and combined frequency and periodicity (Su & Yang 2015),
// a power-compressed spectrum times the generalised cepstrum mapped to frequency.
Spectrogram computeCQT(const std::vector<float> &mono, int binsPerOctave, double filterScale,
                       const std::function<bool(double)> &progress = nullptr);
Spectrogram computeCFP(const std::vector<float> &mono, int binsPerOctave = 36,
                       const std::function<bool(double)> &progress = nullptr);

// Enhanced summary autocorrelation of mono (22050 Hz) per semitone lo..hi, for `frames` frames at
// hop 256. Cancelled: possibly partial and not normalised.
std::vector<float> sacfRoll(const std::vector<float> &mono, int frames, int lo, int hi,
                            const std::function<bool(double)> &progress = nullptr);
