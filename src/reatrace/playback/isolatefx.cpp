#include "playback/isolate.h"

#include <sstream>

// The filter picks each voice's pitch from its changes by its own play_position, so the audio it
// renders ahead (anticipative FX) still switches at the note boundaries. Per voice, a bank of
// band-passes (two in series each, for steeper sides) at the pitch and its overtones, each `width`
// cents wide (constant Q: vibrato stays inside every band; narrower where the overtones crowd
// together). A band another voice already has (unisons, octaves, shared overtones) is used once,
// so it isn't louder. Plus the rest of the signal at the background level. No voice with a pitch:
// everything passes.
std::string isolateJSFX() {
    std::ostringstream s;
    s << "desc:" << ISOLATE_DESC << " (written by the ReaTrace extension)\n"
      << "slider1:100<20,400,1>Width (cents)\n"
      << "slider2:-24<-60,0,1>Background (dB)\n";
    for (int v = 0; v < ISOLATE_VOICES; v++) {
        const int base = isolateParam(v) + 1; // sliders count from 1
        s << "slider" << base << ":0<0," << ISOLATE_CHANGES << ",1>-Voice " << v + 1
          << " changes\n";
        for (int i = 0; i < ISOLATE_CHANGES; i++) {
            s << "slider" << base + 1 + 2 * i << ":0<0,100000,0.0001>-Voice " << v + 1 << " time "
              << i + 1 << "\n"
              << "slider" << base + 2 + 2 * i << ":-1<-1,127,1>-Voice " << v + 1 << " pitch "
              << i + 1 << "\n";
        }
    }
    s << "in_pin:left\nin_pin:right\nout_pin:left\nout_pin:right\n\n@init\n"
      << "VOICES = " << ISOLATE_VOICES << "; STRIDE = " << 1 + 2 * ISOLATE_CHANGES << ";\n"
      << R"(H = 40; // most bands per voice
SLOTS = VOICES * H; // band slots (EEL names ignore case): voice v's overtone k is slot v * H + k
b0 = 0; a1 = SLOTS; a2 = 2 * SLOTS; on = 3 * SLOTS; fc = 4 * SLOTS; // coefficients, whether used, frequency
zl1 = 5 * SLOTS; zl2 = 6 * SLOTS; zl3 = 7 * SLOTS; zl4 = 8 * SLOTS; // two stages of state per channel
zr1 = 9 * SLOTS; zr2 = 10 * SLOTS; zr3 = 11 * SLOTS; zr4 = 12 * SLOTS;
lf = 13 * SLOTS; // each voice's gliding log2 frequency, -1 when it has none
v = 0; loop(VOICES, lf[v] = -1; v += 1);
pass = 1;

@slider
bg = 10 ^ (slider2 / 20);
bw = slider1 / 1200; // octaves

@block
pass = 1;
top = min(0.45 * srate, 12000);
v = 0;
loop(VOICES,
  base = 3 + v * STRIDE;
  cnt = slider(base);
  target = cnt > 0 ? slider(base + 2) : -1; // before the first change: its pitch
  i = 0;
  loop(cnt, slider(base + 1 + 2 * i) <= play_position ? target = slider(base + 2 + 2 * i); i += 1);
  target >= 0 ? (
    pass = 0;
    tlf = (target - 69) / 12 + log(440) / log(2);
    lf[v] < 0 ? lf[v] = tlf : lf[v] += (tlf - lf[v]) * min(1, samplesblock / (0.015 * srate));
  ) : lf[v] = -1;
  k = 0;
  loop(H,
    j = v * H + k;
    f = (2 ^ lf[v]) * (k + 1);
    // at high overtones the bands would overlap and pass everything: at most half the spacing
    bwk = min(bw, 0.5 * log(1 + 1 / (k + 1)) / log(2));
    on[j] = lf[v] >= 0 && f < top;
    m = 0;
    loop(v * H, on[j] && on[m] && abs(log(fc[m] / f)) < 0.5 * bwk * log(2) ? on[j] = 0; m += 1);
    on[j] ? (
      fc[j] = f;
      w = 2 * $pi * f / srate;
      x = log(2) / 2 * bwk * w / sin(w);
      alpha = sin(w) * (exp(x) - exp(-x)) / 2;
      norm = 1 / (1 + alpha);
      b0[j] = alpha * norm; a1[j] = -2 * cos(w) * norm; a2[j] = (1 - alpha) * norm;
    ) : (
      zl1[j] = zl2[j] = zl3[j] = zl4[j] = zr1[j] = zr2[j] = zr3[j] = zr4[j] = 0;
    );
    k += 1;
  );
  v += 1;
);

@sample
pass == 0 ? (
  xl = spl0; xr = spl1; yl = 0; yr = 0; j = 0;
  loop(SLOTS,
    on[j] ? (
      o = b0[j] * xl + zl1[j]; zl1[j] = zl2[j] - a1[j] * o; zl2[j] = -b0[j] * xl - a2[j] * o;
      p = b0[j] * o + zl3[j]; zl3[j] = zl4[j] - a1[j] * p; zl4[j] = -b0[j] * o - a2[j] * p; yl += p;
      o = b0[j] * xr + zr1[j]; zr1[j] = zr2[j] - a1[j] * o; zr2[j] = -b0[j] * xr - a2[j] * o;
      p = b0[j] * o + zr3[j]; zr3[j] = zr4[j] - a1[j] * p; zr4[j] = -b0[j] * o - a2[j] * p; yr += p;
    );
    j += 1;
  );
  spl0 = yl + bg * xl; spl1 = yr + bg * xr;
);
)";
    return s.str();
}
