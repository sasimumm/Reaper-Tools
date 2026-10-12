#include "dsp/basicpitch.h"

#import <CoreML/CoreML.h>

#include <algorithm>
#include <cmath>

// basic-pitch's windowing (basic_pitch/inference.py): 2 s windows of 43844 samples, 30 frames of
// overlap, the audio padded with half the overlap in front, and 15 frames dropped at each window edge.
static const int WINDOW = 43844, HOP = 256, OVERLAP = 30 * HOP, WINDOW_HOP = WINDOW - OVERLAP,
                 OUT_FRAMES = 172, EDGE = 15;

static void setError(std::string *error, const char *what, NSError *e) {
    if (!error) {
        return;
    }
    *error = what;
    if (e) {
        *error += std::string(": ") + e.localizedDescription.UTF8String;
    }
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
        out.note.assign((size_t)out.frames * BasicPitch::PITCHES, 0);
        out.onset.assign(out.note.size(), 0);
        // Output frame j of window w is original sample w*WINDOW_HOP - OVERLAP/2 + j*HOP. Each uniform
        // frame takes the nearest kept output frame of the window covering it, which avoids the drift
        // of reading the unwrapped output as uniform (~10 ms per window).
        const int windows =
            out.frames ? (int)((long long)(out.frames - 1) * HOP / WINDOW_HOP) + 1 : 0;
        std::vector<float> buf(WINDOW);
        int f = 0;
        for (int w = 0; w < windows; w++) {
            // each window's outputs are released here, not kept until all windows are done
            @autoreleasepool {
                const long long start = (long long)w * WINDOW_HOP - OVERLAP / 2;
                for (int i = 0; i < WINDOW; i++) {
                    const long long j = start + i;
                    buf[i] = j >= 0 && j < (long long)mono.size() ? mono[j] : 0;
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
                if (!note || !onset || note.dataType != MLMultiArrayDataTypeFloat32 ||
                    onset.dataType != MLMultiArrayDataTypeFloat32 || note.shape.count != 3 ||
                    note.shape[1].intValue != OUT_FRAMES ||
                    note.shape[2].intValue != BasicPitch::PITCHES || onset.shape.count != 3 ||
                    onset.shape[1].intValue != OUT_FRAMES ||
                    onset.shape[2].intValue != BasicPitch::PITCHES) {
                    setError(error, "unexpected basic-pitch output", e);
                    return false;
                }
                const float *np = (const float *)note.dataPointer,
                            *op = (const float *)onset.dataPointer;
                const long ns1 = note.strides[1].longValue, ns2 = note.strides[2].longValue;
                const long os1 = onset.strides[1].longValue, os2 = onset.strides[2].longValue;
                for (; f < out.frames && (long long)f * HOP < (long long)(w + 1) * WINDOW_HOP;
                     f++) {
                    const int j = std::max(
                        EDGE, std::min(OUT_FRAMES - EDGE - 1,
                                       (int)lround(((double)f * HOP - (double)start) / HOP)));
                    for (int p = 0; p < BasicPitch::PITCHES; p++) {
                        out.note[(size_t)f * BasicPitch::PITCHES + p] = np[j * ns1 + p * ns2];
                        out.onset[(size_t)f * BasicPitch::PITCHES + p] = op[j * os1 + p * os2];
                    }
                }
                if (progress && !progress((double)(w + 1) / windows)) {
                    return false;
                }
            }
        }
        return true;
    }
}
