# ReaTrace

REAPER extension for transcribing audio (mainly vocals) by drawing notes on a spectrogram.
See `docs/piano-roll.md` for the full feature plan.

## Build (macOS)

    git submodule update --init
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target deploy   # copies reaper_reatrace.dylib to UserPlugins
    ./build/nsgt_test && ./build/editor_test && ./build/notesync_test && ./build/contour_test && ./build/estimate_test

Checks while developing:

- Our sources build with `-Wall -Wextra -Wshadow -Wconversion` as errors (`-DREATRACE_WERROR=OFF` to relax),
  and libc++'s hardening mode bounds-checks `std::vector`/`std::span`/`std::string` indexing, also in the plugin.
- Sanitized self-checks, in their own build directories:

      cmake -S . -B build-asan -DREATRACE_SANITIZE=address,undefined && cmake --build build-asan
      cmake -S . -B build-tsan -DREATRACE_SANITIZE=thread && cmake --build build-tsan
      ./build-asan/estimate_test   # and the other *_test programs

- clang-tidy (from Homebrew's `llvm`; checks in `.clang-tidy`): `cmake --build build --target lint`
- clang-format (also from Homebrew's `llvm`; style in `.clang-format`): `cmake --build build --target format`,
  or `format-check` to only report differences.

The build compiles basic-pitch's CoreML model (needs Xcode's `coremlcompiler`); deploy installs it as
`UserPlugins/reatrace/nmp.mlmodelc`.

Restart REAPER after deploying.

Voices: the lead goes on "ReaTrace notes"; voices 2–4 (harmonies, backing) go on "ReaTrace
voice 2".."4" tracks below it, created with their first note, each with ReaSynth.

Settings are remembered per audio item; "Save as default" makes the current ones the starting
point for new items. Presets: "Vocal in mix" (centre channel, noise subtraction, 60 Hz–5 kHz),
"Solo vocal", "Fast runs" (sharper timing). Scroll over the panel to see all settings.

The settings panel also has the analysis channel (Centre = mid minus side, which helps pull a
centre-panned lead vocal out of a mix) and playback speed (100/75/50 %, pitch preserved).

Tempo mapping is left to REAPER's own tools (tempo markers, "create measure from time
selection", SWS tempo tools); the grid follows the project tempo map.

Lyrics are stored as MIDI lyric events at each note's start, so they also show in REAPER's notation
editor and lyrics view.

## Use

Select an audio item and run **ReaTrace: Open spectrogram editor for selected item**. Notes go
into a MIDI item on a "ReaTrace notes" track (with ReaSynth) below the audio track, so they are
saved with the project, can be undone, and can also be edited in REAPER's MIDI editor.

| Input | Action |
|---|---|
| drag on empty space | draw a note |
| drag a note / its edge | move / resize (all selected notes) |
| shift while moving | lock the move to time or pitch, whichever the drag mostly went along |
| click, cmd+click, shift+drag | select, toggle selection, marquee select |
| right-click, Delete/Backspace | delete note / selected notes |
| hover | overtone marks (1–8) for the pitch under the mouse, and the octave check: level at the pitch one octave, a 12th and two octaves below; orange means the real note is probably lower |
| alt while dragging | no snapping; a drawn note otherwise takes the sung pitch under it |
| scroll / shift+scroll / horizontal scroll | scroll pitch / time / time |
| cmd+scroll or pinch, alt+scroll | zoom time, zoom pitch |
| F, Z | fit item, zoom to selection |
| G | snap: onsets (default: note starts/ends snap to detected onsets and pitch changes within 30 ms), grid, off |
| P | pitch contour (white line: the sung pitch) |
| N | suggested notes (dashed; from the note estimator, or the lead contour, chosen in the panel), coloured by the voice they were assigned to; click one to accept it into the active voice |
| A | accept all suggested notes in view, each into the voice it is coloured as (voice tracks are created as needed; grey ones are left) |
| cmd+C, cmd+X | copy, cut selected notes (with their lyrics) |
| cmd+V | after a copy here: paste the notes at the edit cursor. Once other text has been copied: paste lyrics, one syllable per note (split on spaces and after hyphens) onto the selected notes, or from the selected note on |
| cmd+D | duplicate selected notes right after the selection |
| K | split the note under the mouse at the mouse (snapped; alt: not), or the selected notes there; with the mouse outside the notes, the selected notes at the edit cursor |
| J | join selected notes into one (lyrics joined: `hel-` + `lo` = `hello`) |
| S | settings panel |
| D | dock in REAPER's docker / float (remembered) |
| L | link the visible time range to REAPER's arrange view (both ways) |
| 1–4 | edit voice 1 (lead) to 4; the other voices are shown dimmed |
| H | overtone ghosts on/off (faint outlines where each note's overtones should be) |
| X | explain away: dim energy on drawn notes and their overtones, so what is still bright needs transcribing |
| Enter, double-click a note | type its lyric; Tab/Space next note, `-` adds a hyphen and moves on, shift+Tab previous, Enter done, Esc cancel |
| click a piano key | play that pitch through the notes track |
| [ ] , - = , , . | brightness, contrast, resolution |
| click the ruler | move the edit cursor |
| click a syllable in the lyrics lane | type that note's lyric |
| click/drag the overview strip | scroll to that part of the item |
| shift+drag the ruler | time selection and loop points (snapped like notes) |
| B | A/B: hear only the audio / only the notes (track mutes); the panel also has Both and the notes volume |
| R | loop the selected notes' range (or the time selection) from its start; R again stops |
| Q | quantise selected notes to the grid (strength in the panel) |
| other keys | REAPER's own shortcuts (Space = play/stop, ...) |

## Third-party

- [basic-pitch](https://github.com/spotify/basic-pitch) by Spotify, Apache-2.0: its note model
  (`vendor/basic-pitch`, with LICENSE and NOTICE) confirms the note estimator's notes.
- The note estimator follows the approach of the `transcribe` project (docs/NOTE-ESTIMATION.md there).
- REAPER SDK and WDL/SWELL by Cockos.
