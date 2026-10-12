// Self-check for the isolate JSFX (PLAY-9): run `build/isolate_test`. Compiles the JSFX's sections
// with EEL2 (as REAPER does) and runs them on test tones the way REAPER calls them.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "../src/isolate.h"
#include "eel2/ns-eel-int.h" // NSEEL_PProc_THIS
#include "eel2/ns-eel.h"

// EEL2 asks its host for a lock around its global tables; single-threaded here
extern "C" void NSEEL_HOSTSTUB_EnterMutex() {}
extern "C" void NSEEL_HOSTSTUB_LeaveMutex() {}

static const double SR = 48000;
static const int BLOCK = 512;

struct Fx {
    NSEEL_VMCTX vm = NSEEL_VM_alloc();
    std::vector<EEL_F *> sliders; // slider1.. as sliders[0]..
    EEL_F *spl0, *spl1, *srate, *samplesblock, *playPos;
    NSEEL_CODEHANDLE init = nullptr, slider = nullptr, block = nullptr, sample = nullptr;

    static EEL_F NSEEL_CGEN_CALL sliderByIndex(void *self, EEL_F *i) {
        const std::vector<EEL_F *> &s = static_cast<Fx *>(self)->sliders;
        const int k = (int)*i - 1;
        return k >= 0 && k < (int)s.size() ? *s[k] : 0;
    }

    Fx() {
        NSEEL_VM_SetCustomFuncThis(vm, this);
        spl0 = NSEEL_VM_regvar(vm, "spl0"), spl1 = NSEEL_VM_regvar(vm, "spl1");
        srate = NSEEL_VM_regvar(vm, "srate"), samplesblock = NSEEL_VM_regvar(vm, "samplesblock");
        playPos = NSEEL_VM_regvar(vm, "play_position");
        *srate = SR, *samplesblock = BLOCK;
        const std::string src = isolateJSFX();
        // the header: "sliderN:default<...", then "@section" lines start the code sections
        std::string sections[4];
        int cur = -1;
        for (size_t p = 0; p < src.size();) {
            const size_t e = src.find('\n', p);
            const std::string l = src.substr(p, e - p);
            p = e == std::string::npos ? src.size() : e + 1;
            if (l.rfind("slider", 0) == 0 && l.find(':') != std::string::npos && cur < 0) {
                const int n = (int)strtol(l.c_str() + 6, nullptr, 10);
                const std::string name = "slider" + std::to_string(n);
                EEL_F *v = NSEEL_VM_regvar(vm, name.c_str());
                *v = strtod(l.c_str() + l.find(':') + 1, nullptr);
                sliders.resize(std::max(sliders.size(), (size_t)n));
                sliders[(size_t)n - 1] = v;
            }
            static const char *const NAMES[] = {"@init", "@slider", "@block", "@sample"};
            bool header = false;
            for (int s = 0; s < 4; s++) {
                if (l == NAMES[s]) {
                    cur = s, header = true;
                }
            }
            if (!header && cur >= 0) {
                sections[cur] += l + "\n";
            }
        }
        NSEEL_CODEHANDLE *out[] = {&init, &slider, &block, &sample};
        for (int s = 0; s < 4; s++) {
            *out[s] = NSEEL_code_compile_ex(vm, sections[s].c_str(), 0, 0);
            if (!*out[s]) {
                printf("section %d: %s\n", s, NSEEL_code_getcodeerror(vm));
                assert(false);
            }
        }
        NSEEL_code_execute(init);
    }
    ~Fx() {
        for (NSEEL_CODEHANDLE c : {init, slider, block, sample}) {
            NSEEL_code_free(c);
        }
        NSEEL_VM_free(vm);
    }

    // parameters as ReaTrace sends them: width, background, then the (time, pitch) changes
    void set(double width, double bgDb, const std::vector<std::pair<double, int>> &changes) {
        *sliders[0] = width, *sliders[1] = bgDb, *sliders[2] = (double)changes.size();
        for (size_t i = 0; i < changes.size(); i++) {
            *sliders[3 + 2 * i] = changes[i].first, *sliders[4 + 2 * i] = changes[i].second;
        }
        NSEEL_code_execute(slider);
    }

    std::vector<float> run(const std::vector<float> &x) {
        std::vector<float> y(x.size());
        for (size_t b = 0; b < x.size(); b += BLOCK) {
            *playPos = (double)b / SR;
            NSEEL_code_execute(block);
            for (size_t i = b; i < std::min(x.size(), b + BLOCK); i++) {
                *spl0 = *spl1 = x[i];
                NSEEL_code_execute(sample);
                assert(*spl0 == *spl1);
                y[i] = (float)*spl0;
            }
        }
        return y;
    }
};

static double hz(double pitch) { return 440 * pow(2, (pitch - 69) / 12); }

// a sung-ish tone: 1/k harmonics, optional ±cents vibrato at 5.5 Hz
static std::vector<float> tone(double pitch, double seconds, double vibratoCents = 0) {
    std::vector<float> x((size_t)(seconds * SR));
    double ph = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double f =
            hz(pitch) * pow(2, vibratoCents / 1200 * sin(2 * M_PI * 5.5 * (double)i / SR));
        ph += 2 * M_PI * f / SR;
        double s = 0;
        for (int k = 1; k * hz(pitch) < 16000; k++) {
            s += sin(k * ph) / k;
        }
        x[i] = (float)(0.2 * s);
    }
    return x;
}

// level of y relative to x in dB over [a, b) seconds
static double gainDb(const std::vector<float> &x, const std::vector<float> &y, double a, double b) {
    double ex = 0, ey = 0;
    for (size_t i = (size_t)(a * SR); i < (size_t)(b * SR) && i < x.size(); i++) {
        ex += (double)x[i] * x[i], ey += (double)y[i] * y[i];
    }
    return 10 * log10(ey / ex);
}

int main() {
    NSEEL_init();
    NSEEL_addfunc_retval("slider", 1, NSEEL_PProc_THIS, &Fx::sliderByIndex);

    // no changes: everything passes untouched
    {
        Fx fx;
        fx.set(100, -24, {});
        const std::vector<float> x = tone(57, 0.5), y = fx.run(x);
        assert(x == y);
    }
    // following A3: A3 (with vibrato) is kept, a note a semitone or a minor third away is not
    {
        Fx fx;
        fx.set(100, -60, {{0, 57}});
        const std::vector<float> a = tone(57, 1.5, 40), b = tone(58, 1.5), c = tone(60, 1.5);
        const double ka = gainDb(a, fx.run(a), 0.5, 1.5);
        Fx fb, fc;
        fb.set(100, -60, {{0, 57}}), fc.set(100, -60, {{0, 57}});
        const double kb = gainDb(b, fb.run(b), 0.5, 1.5), kc = gainDb(c, fc.run(c), 0.5, 1.5);
        printf("isolate A3: A3 with vibrato %.1f dB, A#3 %.1f dB, C4 %.1f dB\n", ka, kb, kc);
        assert(ka > -2 && kb < -10 && kc < -10);
    }
    // a pitch change at 1 s: before it C4 is filtered out, after it (and the glide) C4 passes
    {
        Fx fx;
        fx.set(100, -60, {{0, 57}, {1, 60}});
        const std::vector<float> c = tone(60, 2), y = fx.run(c);
        const double before = gainDb(c, y, 0.3, 0.95), after = gainDb(c, y, 1.1, 2);
        printf("isolate change at 1 s: C4 %.1f dB before, %.1f dB after\n", before, after);
        assert(before < -10 && after > -2);
    }
    // the background level: a tone between the bands comes through at about that level
    {
        Fx fx;
        fx.set(100, -24, {{0, 57}});
        std::vector<float> x((size_t)SR);
        for (size_t i = 0; i < x.size(); i++) {
            x[i] = (float)(0.3 * sin(2 * M_PI * hz(63) * (double)i / SR)); // a tritone above A3
        }
        const double g = gainDb(x, fx.run(x), 0.3, 1);
        printf("isolate background: %.1f dB\n", g);
        assert(fabs(g + 24) < 3);
    }
    printf("ok\n");
}
