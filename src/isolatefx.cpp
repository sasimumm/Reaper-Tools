#include "isolate.h"

#include <sstream>

// The filter picks its pitch from the changes by its own play_position, so the audio it renders
// ahead (anticipative FX) still switches at the note boundaries. A bank of band-passes (two in
// series each, for steeper sides) at the pitch and its overtones, each `width` cents wide
// (constant Q: vibrato stays inside every band; narrower where the overtones crowd together), plus
// the rest of the signal at the background level. Pitch -1: everything passes.
std::string isolateJSFX() {
    std::ostringstream s;
    s << "desc:" << ISOLATE_DESC << " (written by the ReaTrace extension)\n"
      << "slider1:100<20,400,1>Width (cents)\n"
      << "slider2:-24<-60,0,1>Background (dB)\n"
      << "slider3:0<0," << ISOLATE_CHANGES << ",1>-Changes\n";
    for (int i = 0; i < ISOLATE_CHANGES; i++) {
        s << "slider" << 4 + 2 * i << ":0<0,100000,0.0001>-Time " << i + 1 << "\n"
          << "slider" << 5 + 2 * i << ":-1<-1,127,1>-Pitch " << i + 1 << "\n";
    }
    s << R"(in_pin:left
in_pin:right
out_pin:left
out_pin:right

@init
H = 40; // most bands
b0 = 0; a1 = H; a2 = 2 * H; // coefficients, then two stages of state per channel
zl1 = 3 * H; zl2 = 4 * H; zl3 = 5 * H; zl4 = 6 * H; zr1 = 7 * H; zr2 = 8 * H; zr3 = 9 * H; zr4 = 10 * H;
lf = -1; n = -1;

@slider
bg = 10 ^ (slider2 / 20);
bw = slider1 / 1200; // octaves

@block
target = slider3 > 0 ? slider(5) : -1; // before the first change: its pitch
i = 0;
loop(slider3, slider(4 + 2 * i) <= play_position ? target = slider(5 + 2 * i); i += 1);
target >= 0 ? (
  tlf = (target - 69) / 12 + log(440) / log(2);
  lf < 0 ? lf = tlf : lf += (tlf - lf) * min(1, samplesblock / (0.015 * srate)); // 15 ms glide
  f0 = 2 ^ lf;
  top = min(0.45 * srate, 12000);
  n = 0;
  loop(H,
    f0 * (n + 1) < top ? (
      w = 2 * $pi * f0 * (n + 1) / srate;
      // at high overtones the bands would overlap and pass everything: at most half the spacing
      x = log(2) / 2 * min(bw, 0.5 * log(1 + 1 / (n + 1)) / log(2)) * w / sin(w);
      alpha = sin(w) * (exp(x) - exp(-x)) / 2;
      norm = 1 / (1 + alpha);
      b0[n] = alpha * norm; a1[n] = -2 * cos(w) * norm; a2[n] = (1 - alpha) * norm;
      n += 1;
    );
  );
) : (
  n = -1; lf = -1;
);

@sample
n >= 0 ? (
  xl = spl0; xr = spl1; yl = 0; yr = 0; j = 0;
  loop(n,
    o = b0[j] * xl + zl1[j]; zl1[j] = zl2[j] - a1[j] * o; zl2[j] = -b0[j] * xl - a2[j] * o;
    p = b0[j] * o + zl3[j]; zl3[j] = zl4[j] - a1[j] * p; zl4[j] = -b0[j] * o - a2[j] * p; yl += p;
    o = b0[j] * xr + zr1[j]; zr1[j] = zr2[j] - a1[j] * o; zr2[j] = -b0[j] * xr - a2[j] * o;
    p = b0[j] * o + zr3[j]; zr3[j] = zr4[j] - a1[j] * p; zr4[j] = -b0[j] * o - a2[j] * p; yr += p;
    j += 1;
  );
  spl0 = yl + bg * xl; spl1 = yr + bg * xr;
);
)";
    return s.str();
}
