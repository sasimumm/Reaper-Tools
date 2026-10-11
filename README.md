# Reascribe

REAPER extension for transcribing audio (mainly vocals) by drawing notes on a spectrogram.
See `docs/piano-roll.md` for the full feature plan.

## Build (macOS)

    git submodule update --init
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target deploy   # copies reaper_reascribe.dylib to UserPlugins
    ./build/nsgt_test && ./build/editor_test

Restart REAPER after deploying.

Lyrics are stored as MIDI lyric events at each note's start, so they also show in REAPER's notation
editor and lyrics view.

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
| hover | overtone marks (1–8) for the pitch under the mouse, and the octave check: level at the pitch one octave, a 12th and two octaves below; orange means the real note is probably lower |
| alt while dragging | no snapping |
| scroll / shift+scroll / horizontal scroll | scroll pitch / time / time |
| cmd+scroll or pinch, alt+scroll | zoom time, zoom pitch |
| F, Z | fit item, zoom to selection |
| G | snap to grid on/off (off by default for free-tempo recordings) |
| S | settings panel |
| H | overtone ghosts on/off (faint outlines where each note's overtones should be) |
| Enter, double-click a note | type its lyric; Tab/Space next note, shift+Tab previous, Enter done, Esc cancel |
| click a piano key | play that pitch through the notes track |
| [ ] , - = , , . | brightness, contrast, resolution |
| click the ruler | move the edit cursor; Space plays (REAPER) |
