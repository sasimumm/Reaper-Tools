// basic-pitch through CoreML: runs the model window by window and resamples its output frames
// onto the uniform 256-sample hop.
#include "dsp/basicpitch.h"

#import <CoreML/CoreML.h>

#include "index.h"
#include <algorithm>
#include <cmath>
#include <span>

// basic-pitch's windowing (basic_pitch/inference.py): 2 s windows of 43844 samples, 30 frames of
// overlap, the audio padded with half the overlap in front, and 15 frames dropped at each window
// edge.
static const int WINDOW = 43844; // samples per model input
static const int HOP = 256;      // samples per output frame
static const int OVERLAP = 30 * HOP;
static const int WINDOW_HOP = WINDOW - OVERLAP;
static const int OUT_FRAMES = 172; // output frames per window
static const int EDGE = 15;        // output frames dropped at each window edge

// Sets *error (if wanted) to `what`, followed by e's description if there is one.
static void setError(std::string *error, const char *what, NSError *e) {
    if (!error) {
        return;
    }
    *error = what;
    if (e) {
        *error += std::string(": ") + e.localizedDescription.UTF8String;
    }
}

// The model's note and onset outputs are 1 x OUT_FRAMES x PITCHES float32 arrays.
static bool expectedOutput(MLMultiArray *a) {
    return a.dataType == MLMultiArrayDataTypeFloat32 && a.shape.count == 3 &&
           a.shape[1].intValue == OUT_FRAMES && a.shape[2].intValue == BasicPitch::PITCHES;
}

// The first batch's elements of an expectedOutput() array, whose frame f, pitch p is at
// f * strides[1] + p * strides[2].
static std::span<const float> firstBatch(MLMultiArray *a) {
    const long s1 = a.strides[1].longValue;
    const long s2 = a.strides[2].longValue;
    // CoreML hands out bare pointers.
#pragma clang unsafe_buffer_usage begin
    return std::span<const float>(static_cast<const float *>(a.dataPointer),
                                  idx((OUT_FRAMES - 1) * s1 + (BasicPitch::PITCHES - 1) * s2 + 1));
#pragma clang unsafe_buffer_usage end
}

bool runBasicPitch(const std::string &modelPath, const std::vector<float> &mono, int frames,
                   BasicPitch &out, std::string *error,
                   const std::function<bool(double)> &progress) {
    @autoreleasepool {
        NSError *e = nil;
        MLModelConfiguration *config = [[MLModelConfiguration alloc] init];
        config.computeUnits = MLComputeUnitsCPUOnly; // as basic-pitch itself runs it
        NSString *path = [NSString stringWithUTF8String:modelPath.c_str()]; // nil if not UTF-8
        if (!path) {
            setError(error, "the model path isn't valid UTF-8", nil);
            return false;
        }
        NSURL *url = [NSURL fileURLWithPath:path];
        MLModel *model = [MLModel modelWithContentsOfURL:url configuration:config error:&e];
        if (!model) {
            setError(error, "can't load the basic-pitch model", e);
            return false;
        }

        out.frames = std::max(0, frames);
        out.note.assign(idx(out.frames) * BasicPitch::PITCHES, 0);
        out.onset.assign(out.note.size(), 0);

        // Output frame j of window w is original sample w*WINDOW_HOP - OVERLAP/2 + j*HOP. Each
        // uniform frame takes the nearest kept output frame of the window covering it, which
        // avoids the drift of reading the unwrapped output as uniform (~10 ms per window).
        int windows = 0;
        if (out.frames) {
            const long long lastSample = static_cast<long long>(out.frames - 1) * HOP;
            windows = static_cast<int>(lastSample / WINDOW_HOP) + 1;
        }
        std::vector<float> buf(WINDOW);
        int f = 0; // next uniform frame to fill
        for (int w = 0; w < windows; w++) {
            // each window's outputs are released here, not kept until all windows are done
            @autoreleasepool {
                const long long start = static_cast<long long>(w) * WINDOW_HOP - OVERLAP / 2;
                for (int i = 0; i < WINDOW; i++) {
                    const long long j = start + i;
                    buf[idx(i)] =
                        j >= 0 && j < static_cast<long long>(mono.size()) ? mono[idx(j)] : 0;
                }

                MLMultiArray *in =
                    [[MLMultiArray alloc] initWithDataPointer:buf.data()
                                                        shape:@[ @1, @(WINDOW), @1 ]
                                                     dataType:MLMultiArrayDataTypeFloat32
                                                      strides:@[ @(WINDOW), @1, @1 ]
                                                  deallocator:nil
                                                        error:&e];
                if (!in) {
                    setError(error, "can't make the model input", e);
                    return false;
                }
                MLDictionaryFeatureProvider *features =
                    [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"input_2" : in}
                                                                      error:&e];
                id<MLFeatureProvider> result =
                    features ? [model predictionFromFeatures:features error:&e] : nil;
                MLMultiArray *note = [result featureValueForName:@"Identity_1"].multiArrayValue;
                MLMultiArray *onset = [result featureValueForName:@"Identity_2"].multiArrayValue;
                if (!note || !onset || !expectedOutput(note) || !expectedOutput(onset)) {
                    setError(error, "unexpected basic-pitch output", e);
                    return false;
                }

                const long ns1 = note.strides[1].longValue, ns2 = note.strides[2].longValue;
                const long os1 = onset.strides[1].longValue, os2 = onset.strides[2].longValue;
                const std::span<const float> np = firstBatch(note);
                const std::span<const float> op = firstBatch(onset);

                // the uniform frames before the next window's start
                const long long windowEnd = static_cast<long long>(w + 1) * WINDOW_HOP;
                for (; f < out.frames && static_cast<long long>(f) * HOP < windowEnd; f++) {
                    const double outFrame =
                        (static_cast<double>(f) * HOP - static_cast<double>(start)) / HOP;
                    const int j = std::max(
                        EDGE, std::min(OUT_FRAMES - EDGE - 1, static_cast<int>(lround(outFrame))));
                    for (int p = 0; p < BasicPitch::PITCHES; p++) {
                        out.note[idx(f, BasicPitch::PITCHES, p)] = np[idx(j * ns1 + p * ns2)];
                        out.onset[idx(f, BasicPitch::PITCHES, p)] = op[idx(j * os1 + p * os2)];
                    }
                }

                if (progress && !progress(static_cast<double>(w + 1) / windows)) {
                    return false;
                }
            }
        }
        return true;
    }
}
