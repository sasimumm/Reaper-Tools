# Reascribe

REAPER extension that draws a spectrogram of an audio item over REAPER's MIDI editor piano
roll, so you can transcribe by drawing notes on top of it with the normal MIDI editor.

## Build (macOS)

    git submodule update --init
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target deploy   # copies reaper_reascribe.dylib to UserPlugins
    ./build/nsgt_test                     # spectrogram self-check

Restart REAPER after deploying.

## Use

Select an audio item and run **Reascribe: Transcribe selected audio item**. It creates a
"Reascribe notes" track (with ReaSynth) below the audio track, an empty MIDI item over the
audio, and opens it in the MIDI editor with the spectrogram overlaid. Any MIDI item on a
"Reascribe notes" track gets the overlay of the audio on the track above it.

Toggle actions: **Toggle spectrogram overlay**, **Toggle spectrogram peaks only**,
**Toggle spectrogram noise subtraction**.

## Tuning

Constants at the top of `src/main.cpp` (`RULER_H`, `OVERLAY_OPACITY`) and `NSGTParams` in
`src/nsgt.h` (`gamma`: lower = sharper pitch, blurrier timing). If the spectrogram is
vertically offset from the note rows, adjust `RULER_H`.
