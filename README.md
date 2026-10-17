# REAPER extensions

C++ extensions for [REAPER](https://www.reaper.fm), built from one repository. Each one is its own
`reaper_<name>.dylib` in REAPER's UserPlugins folder and works without the others.

| Extension | What it does |
|---|---|
| [ReaTrace](#reatrace) | Transcribe audio (mainly vocals, including harmonies) by drawing notes and lyrics on a spectrogram; the notes are MIDI items in the project. |
| [ReaChords](#reachords) | Chord items: a track of chord names drawn and edited in the arrange view, following tempo. |

macOS only so far.

## Build and install

    git submodule update --init
    make deploy   # builds every extension and copies it to UserPlugins
    make test     # the self-checks

Restart REAPER after deploying. `make help` lists the rest; they wrap the CMake invocations
(in `build/`):

- `make asan`, `make tsan`: the self-checks with sanitizers, in `build-asan/` and `build-tsan/`.
- `make lint`: clang-tidy (from Homebrew's `llvm`; checks in `.clang-tidy`).
- `make format`, `make format-check`: clang-format (also Homebrew's `llvm`; style in `.clang-format`).

Our sources build with strict warnings as errors (`-DREATRACE_WERROR=OFF` to relax), among them
`-Wpedantic`, `-Wconversion`, `-Wsign-conversion`, `-Wdouble-promotion`, `-Wold-style-cast` and
`-Wcomma`; the list is in `CMakeLists.txt`, and vendor code is left out. `make lint` fails on any
clang-tidy finding. libc++'s hardening mode bounds-checks `std::vector`/`std::span`/`std::string`
indexing, also in the extensions.

## Layout

- `src/<extension>/`: one folder per extension, with its own `main.cpp` (the entry point and the
  REAPER API functions it imports).
- `src/shared/`: code the extensions share (`guard.h`: exceptions never reach REAPER).
- `tests/`: self-check programs (`build/*_test`), for the parts that don't need REAPER.
- `vendor/`: REAPER SDK, WDL, basic-pitch.

To add an extension: a folder under `src/`, a `reaper_<name>` MODULE target in `CMakeLists.txt`
(like `reaper_reachords`), and that target in the `deploy` copy. Extensions talk to each other only
through REAPER's API table (`rec->Register("API_Name", fn)` on one side, `plugin_getapi("Name")` on
the other), so either can be installed alone.

## ReaTrace

Right-click an audio item and choose **Open in ReaTrace** (or Extensions › ReaTrace, or the action
**ReaTrace: Open spectrogram editor for selected item**). Notes go
into a MIDI item on a "ReaTrace notes" track (with ReaSynth) below the audio track, so they are
saved with the project, can be undone, and can also be edited in REAPER's MIDI editor.

- Voices: the lead goes on "ReaTrace notes"; voices 2–4 (harmonies, backing) go on "ReaTrace
  voice 2".."4" tracks below it, created with their first note, each with ReaSynth. The tracks
  can be renamed; they're found by position (directly below the audio track) and a tag, not name.
- Suggestions come from a note estimator (harmonic-template NMF on the spectrogram, confirmed by
  basic-pitch), assigned to voices; accept them one by one or all in view.
- Reference: after playing a rough take in on a MIDI keyboard, select its track (or a folder of
  tracks) in REAPER and click **Reference** in the toolbar. Its notes then show as suggestions,
  the same as the estimator's, to accept or redraw for the final pass. One track suggests for the
  edited voice; with several, the first goes to the lead, the next to voice 2 and so on. Clicking
  Reference again with other tracks selected switches to those.
- Lyrics are stored as MIDI lyric events at each note's start, so they also show in REAPER's
  notation editor and lyrics view.
- Settings are remembered per audio item; "Save as default" makes the current ones the starting
  point for new items. Presets: "Vocal in mix" (centre channel, noise subtraction, 60 Hz–5 kHz),
  "Solo vocal", "Fast runs" (sharper timing). The analysis channel "Centre" (mid minus side) helps
  pull a centre-panned lead vocal out of a mix; playback can be slowed to 75/50 % with pitch kept.
- Tempo mapping is left to REAPER's own tools (tempo markers, "create measure from time
  selection", SWS tempo tools); the grid follows the project tempo map.
- With ReaChords installed, the chord items' chords show in a lane under the ruler.

### Code

`src/reatrace/`: `main.cpp` (entry point, voice tracks), `state.h` (the session's shared
state), `model.*` (settings, layout and other REAPER-free logic), `analysis.*` and `settings.*`
(the analysis job and per-item settings), and `dsp/` (spectrogram, pitch contour, note
estimator, basic-pitch; REAPER-free), `notes/` (editing, notes <-> MIDI take sync, lyrics,
commands), `playback/` (previews, the isolate filter), `ui/` (window, painting, panel, toolbar, theme colours).
The build compiles basic-pitch's CoreML model (needs Xcode's `coremlcompiler`); deploy installs it
as `UserPlugins/reatrace/nmp.mlmodelc`.

### Keys and mouse

| Input | Action |
|---|---|
| drag on empty space | draw a note |
| drag a note / its edge | move / resize (all selected notes) |
| shift while moving | lock the move to time or pitch, whichever the drag mostly went along |
| click, cmd+click, shift+drag | select, toggle selection, marquee select |
| cmd+drag a note | copy the selected notes (and that one) and move the copies, by whole grid steps in beats so they keep their place in the bar across tempo changes (no onset snapping; snap off or alt: freely) |
| shift+I | add intervals, as in Dorico: type `3` (a third above, in the key), `-3` (below), `m3`, `M6`, `P5`, `aug4`, `dim5`, several as `3,5`, then Enter. Above goes from the highest selected note at each time, below from the lowest, so `3` again stacks another third. `t3` or `t-m2` transposes the selected notes instead. The key is the one shown on the keyboard (C major without one) |
| right-click, Delete/Backspace | delete note / selected notes |
| hover | overtone marks (1–8) for the pitch under the mouse, and the octave check: level at the pitch one octave, a 12th and two octaves below; orange means the real note is probably lower |
| alt while dragging | no snapping |
| scroll / shift+scroll / horizontal scroll | scroll pitch / time / time |
| cmd+scroll or pinch, alt+scroll | zoom time, zoom pitch |
| F, Z | fit item, zoom to selection |
| G | snap: onsets (default: note starts/ends snap to detected onsets and pitch changes within 30 ms), grid, off |
| P | pitch contour (yellow line: the sung pitch) |
| C | chord tones: shade each chord's notes (from the chords lane) under it, the root stronger; notes outside the chord get a light dot in their corner (the edited voice's also a darker fill) |
| key | the MIDI take's scale (REAPER's key snap), else the project's key signature as last saved (the first one; key changes aren't read yet): its notes are marked on the keyboard (the root wider), their rows are faintly lighter, and the status bar shows it |
| N | suggested notes (dashed; from the note estimator, or the lead contour, chosen in the panel), coloured by the voice they were assigned to; click one to accept it into the active voice, drag over one to draw |
| A | accept all suggested notes in view, each into the voice it is coloured as (voice tracks are created as needed; grey ones are left) |
| cmd+C, cmd+X | copy, cut selected notes (with their lyrics) |
| cmd+V | after a copy here: paste the notes at the edit cursor. Once other text has been copied: paste lyrics, one syllable per note (split on spaces and after hyphens) onto the selected notes, or from the selected note on |
| cmd+D | duplicate selected notes right after the selection |
| K | split the note under the mouse at the mouse (snapped; alt: not), or the selected notes there; with the mouse outside the notes, the selected notes at the edit cursor |
| J | join selected notes into one (lyrics joined: `hel-` + `lo` = `hello`) |
| S | settings panel (its sections fold at their headers) |
| toolbar | voice (shift-click a voice to hide it), snap, suggestions and their source, contour/ghosts/explain, what you hear and the speed; ? shows every shortcut |
| D | dock in REAPER's docker / float (remembered) |
| L | link the visible time range to REAPER's arrange view (both ways) |
| 1–4 | edit voice 1 (lead) to 4; the other voices are shown dimmed |
| cmd+1–4 | move the selected notes to that voice (with their lyrics) and edit it |
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
| arrows, shift+up/down | nudge selected notes a grid step / a semitone / an octave (without a selection the arrows go to REAPER) |
| T | follow playback: page the view along with the play cursor (on by default) |
| I, alt+I | isolate: hear only the edited voice's pitch and overtones (a JSFX on the audio track); alt+I follows every shown voice |
| U | duck notes: the same filter in reverse, lowering every shown voice's drawn notes and their overtones, to hear what isn't notated yet |
| double-click a panel slider | reset it to its default |
| other keys | REAPER's own shortcuts (Space = play/stop, ...) |

## ReaChords

**Insert › New Chords Item** (also Extensions › ReaChords, or the action **ReaChords: Insert chord
item**) adds a chord item on the selected track, over the time selection, else four bars from the
edit cursor. Double-click in it to type a chord (Enter saves, Esc cancels,
empty deletes); drag the lines between chords. Chords are kept in quarter notes from the item's
start, so they follow tempo changes.

Other extensions read the project's chords through REAPER's API:
`int ReaChords_GetChord(int index, double *start, double *end, char *name, int nameSize)` gives the
index-th chord by start (project seconds, clipped to its item), 0 past the last one.

`src/reachords/`: `main.cpp` (entry point, the exported API), `chords.*` (the chord item
source, drawing and editing).

## Third-party

- [basic-pitch](https://github.com/spotify/basic-pitch) by Spotify, Apache-2.0: its note model
  (`vendor/basic-pitch`, with LICENSE and NOTICE) confirms the note estimator's notes.
- The note estimator follows the approach of the `transcribe` project (docs/NOTE-ESTIMATION.md there).
- REAPER SDK and WDL/SWELL/EEL2 by Cockos.
