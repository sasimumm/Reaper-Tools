# Reascribe

REAPER extension for transcribing audio (mainly vocals) by drawing notes on a spectrogram.
See `docs/piano-roll.md` for the full feature plan.

## Build (macOS)

    git submodule update --init
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target deploy   # copies reaper_reascribe.dylib to UserPlugins
    ./build/nsgt_test && ./build/editor_test

Restart REAPER after deploying.

## Use

Select an audio item and run **Reascribe: Open spectrogram editor for selected item**. Notes go
into a MIDI item on a "Reascribe notes" track (with ReaSynth) below the audio track, so they are
saved with the project, can be undone, and can also be edited in REAPER's MIDI editor.

| Input | Action |
|---|---|
| drag on empty space | draw a note |
| drag a note / its edge | move / resize (all selected notes) |
| click, cmd+click, shift+drag | select, toggle selection, marquee select |
| right-click, Delete/Backspace | delete note / selected notes |
| alt while dragging | no snapping |
| scroll / shift+scroll / horizontal scroll | scroll pitch / time / time |
| cmd+scroll or pinch, alt+scroll | zoom time, zoom pitch |
| F, Z | fit item, zoom to selection |
| G | snap to grid on/off (off by default for free-tempo recordings) |
| S | settings panel |
| [ ] , - = , , . | brightness, contrast, resolution |
| click the ruler | move the edit cursor; Space plays (REAPER) |
