# Reascribe

REAPER extension for transcribing audio (mainly vocals) by drawing notes on a spectrogram.
See `docs/piano-roll.md` for the full feature plan.

## Build (macOS)

    git submodule update --init
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target deploy   # copies reaper_reascribe.dylib to UserPlugins
    ./build/nsgt_test && ./build/editor_test

Restart REAPER after deploying.

Settings are remembered per audio item; "Save as default" makes the current ones the starting
point for new items. Presets: "Vocal in mix" (centre channel, noise subtraction, 60 Hz–5 kHz),
"Solo vocal", "Fast runs" (sharper timing). Scroll over the panel to see all settings.

The settings panel also has the analysis channel (Centre = mid minus side, which helps pull a
centre-panned lead vocal out of a mix) and playback speed (100/75/50 %, pitch preserved).

Fitting the tempo map to a free-tempo recording: shift+drag over the first bar in the ruler and
press B, then adjust the next selection and press B again, bar by bar (or use "Selection = 1 beat"
and "Tempo marker at edit cursor" in the panel). These keep the recording, notes and lyrics where
they are; only the grid moves. The notes item uses the time timebase so REAPER's own tempo edits
don't move it either.

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
| alt while dragging | no snapping; a drawn note otherwise takes the sung pitch under it |
| scroll / shift+scroll / horizontal scroll | scroll pitch / time / time |
| cmd+scroll or pinch, alt+scroll | zoom time, zoom pitch |
| F, Z | fit item, zoom to selection |
| G | snap: onsets (default: note starts/ends snap to detected onsets and pitch changes within 30 ms), grid, off |
| P | pitch contour (white line: the sung pitch) |
| N | suggested notes (dashed: stable stretches of the contour); click one to accept it |
| A | accept all suggested notes in view |
| cmd+V | paste lyrics: one syllable per note (split on spaces and after hyphens) onto the selected notes, or from the selected note on |
| S | settings panel |
| H | overtone ghosts on/off (faint outlines where each note's overtones should be) |
| X | explain away: dim energy on drawn notes and their overtones, so what is still bright needs transcribing |
| Enter, double-click a note | type its lyric; Tab/Space next note, `-` adds a hyphen and moves on, shift+Tab previous, Enter done, Esc cancel |
| click a piano key | play that pitch through the notes track |
| [ ] , - = , , . | brightness, contrast, resolution |
| click the ruler | move the edit cursor |
| shift+drag the ruler | time selection (snapped like notes) |
| B | the time selection is one bar: fits the tempo map to it, then selects the next bar |
| Q | quantise selected notes to the grid (strength in the panel) |
| other keys | REAPER's own shortcuts (Space = play/stop, ...) |
