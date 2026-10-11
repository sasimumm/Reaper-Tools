# ReaTrace piano roll: requirements and status

A custom REAPER window for transcribing audio by drawing notes on a spectrogram.
Priorities: **P0** = first usable version, **P1** = makes transcription fast, **P2** = later / experimental.
Status: ✅ done, ◐ partly done (see note), — not started, ✗ dropped.
IDs are stable: new items get the next free number in their section, and removed numbers aren't reused.

Target use:

- **Vocals**, usually one lead line, sometimes harmonies or backing vocals, often inside a full mix.
- **Free-tempo recordings** (no click). The grid only means something once a tempo map has been
  fitted with REAPER's own tools (§6), so pitch and onset aids matter more than grid snapping.
- **Velocity doesn't matter:** notes get a fixed velocity.
- **REAPER MIDI is the end product**, plus **lyrics** entered in the same view (§5a).

Guiding principles (several learned the hard way in DrawScore):

- **The MIDI item is the source of truth.** Notes live in a normal REAPER MIDI take, so they are saved
  with the project, undoable, and editable in REAPER's own MIDI editor (velocity lanes, CC, quantize).
  The window re-reads the take when it changes elsewhere.
- **No modes.** Dragging on empty space draws, dragging a note moves it, dragging an edge resizes it.
  (DrawScore removed its edit-mode toggle for this reason.)
- **Draw order is ours:** spectrogram → grid → overtone guides → notes → selection/drag feedback → cursors.
  Notes must always stay readable over a bright spectrogram. (An overlay on REAPER's own MIDI
  editor was tried first and dropped: it can only draw on top of REAPER's notes, tinting them, and
  playback left traces.)
- **Reuse REAPER** for transport, tempo map, grid, undo, synth and playback rate instead of rebuilding them.

---

## 1. Window and layout

| ID | | Status | Item |
|---|---|---|---|
| WIN-1 | P0 | ✅ | Resizable window, opened by an action on the selected audio item (or the selected ReaTrace MIDI item). |
| WIN-2 | P0 | ✅ | Layout: piano keyboard on the left, time ruler on top (bars/beats from the tempo map, plus seconds), spectrogram+notes in the middle, status line (pitch name, Hz, cents off, time, bar.beat under the mouse). The status line also shows the nearest peak's cents, the level in dB, the hovered note's sung offset, the snap mode and analysis progress. |
| WIN-3 | P0 | ✅ | Window receives keyboard shortcuts (needs REAPER's `accelerator` hook, since REAPER grabs keys first). All keys the window doesn't use go to REAPER's main shortcuts, so Space etc. behave as in REAPER. |
| WIN-4 | P1 | ◐ | Dockable in REAPER's docker (`DockWindowAddEx`), position and size remembered. Docking (D) and the docked/floating choice are remembered; the floating window's position and size are not. |
| WIN-5 | P1 | ✅ | Retina: render the spectrogram and notes at 2× backing resolution. |
| WIN-6 | P1 | ✅ | Colours follow the REAPER theme or a built-in dark theme (panel toggle). The window, ruler, panel and text use the theme's main window and timeline colours; the note area stays dark under the spectrogram, and notes keep their voice colours so they stay readable on it. |
| WIN-7 | P2 | — | Optional waveform strip under the ruler, for spotting onsets. |
| WIN-8 | P2 | — | Windows support (dialog template and DIB section instead of SWELL-only calls). |

## 2. Navigation

| ID | | Status | Item |
|---|---|---|---|
| NAV-1 | P0 | ✅ | Horizontal zoom around the mouse and horizontal scroll. Mapped for trackpads: scroll pans pitch, horizontal or shift+scroll pans time, cmd+scroll or pinch zooms time. |
| NAV-2 | P0 | ◐ | Vertical zoom and scroll: alt+scroll and alt+pinch zoom pitch. Directional pinch isn't possible: macOS only reports the zoom amount, not its direction. |
| NAV-3 | P0 | ✅ | Zoom to fit the item (F); zoom to selection (Z). |
| NAV-4 | P1 | — | Follow playback (page or continuous scroll), toggleable. |
| NAV-5 | P1 | ✅ | Keep the view in sync with REAPER's arrange view (optional link, L or the panel). Time range only: when linked, the window starts by following the arrange view, then whichever side scrolls or zooms moves the other. |
| NAV-6 | P1 | — | Trackpad pan with inertia (DrawScore: ×0.95 per frame, stop below 0.5 px/s). |
| NAV-7 | P2 | ✅ | Mini overview of the whole item with the visible range: a strip above the status line with the spectrogram's vocal range (E2 … C6) and the active voice's notes; click or drag it to scroll there. |

## 3. Spectrogram display

| ID | | Status | Item |
|---|---|---|---|
| SPEC-1 | P0 | ✅ | NSGT spectrogram (`src/nsgt.cpp`), computed in 32768-sample blocks (WDL's FFT limit), cached, mapped to the view with max-pooling. |
| SPEC-2 | P0 | ✅ | Pitch axis aligned so each note row is centred on its pitch's frequency (DrawScore: notes span pitch ±0.5 semitone, interpolated over bin centres). |
| SPEC-3 | P0 | ✅ | Auto-calibrated contrast as the starting point; every parameter is adjustable (see §3a). |
| SPEC-4 | P1 | ✅ | Recompute in the background with progress, so long items don't freeze REAPER. Cached per channel, keyed by the analysis settings and the item's position, length, offset and playrate; not saved to disk. |
| SPEC-5 | P1 | ✅ | Optional second "bass" analysis with longer windows, blended below a crossover pitch. Panel toggle plus a crossover slider (80 … 500 Hz, default 200 Hz). The bass pass uses the longest windows a 32768-sample block allows (about 3 Hz bandwidth) and cross-fades over half an octave below the crossover; it adds ~70 ms per 3 minutes. Two low notes a semitone apart separate clearly (self-checked). |
| SPEC-6 | P1 | ◐ | Display filters: peaks only and noise subtraction are done; visible-window normalisation (DrawScribe) is not. |
| SPEC-7 | P1 | ◐ | Recompute automatically when the audio item's offset, length, position or playrate changes (debounced 0.3 s); a changed source file isn't detected. |
| SPEC-8 | P1 | ✅ | Channel choice: mix (= mid), left, right, side, and centre (mid magnitudes minus side magnitudes, which pulls out a centre-panned lead vocal). |
| SPEC-9 | P2 | — | Harmonic/percussive separation (median filtering) to show tonal content without drum smear. |
| SPEC-10 | P2 | — | Spectral whitening / per-bin normalisation for quiet high parts. |

## 3a. Spectrogram controls

A settings panel (side panel, S; scrolls with the wheel) with a slider per parameter. Display
changes apply instantly from the cached magnitudes (recolouring 3 minutes takes ~40–60 ms through a
lookup table); analysis changes recompute in the background when the slider is released. Number
fields next to the sliders are not done.

**Display parameters** (instant, P0 unless noted):

| ID | Parameter | Range | Default | Effect | Status |
|---|---|---|---|---|---|
| CTL-1 | dB floor | −120 … −20 dB | auto (p30) | level that maps to black/transparent | ✅ |
| CTL-2 | Contrast | 0.1 … 3 | auto (from p95) | spread of the colour range | ✅ |
| CTL-3 | Brightness | −1 … 1 | auto (−0.10) | shifts everything up or down | ✅ |
| CTL-4 | Colour curve (bias) | −2 … 2 | −0.8 | gamma of the colormap; negative darkens quiet parts | ✅ |
| CTL-5 | Colormap | magma, inferno, viridis, plasma, cividis, grey | magma | P1 | — magma only |
| CTL-6 | Peaks only | on/off, dim factor 0 … 1 | off, 0.15 | keep per-frame frequency peaks, dim the rest | ✅ |
| CTL-7 | Noise subtraction | on/off, strength 0 … 3, noise percentile 1 … 50% | off, 1.5, 10% | subtract the quiet-frame spectrum | ✅ |
| CTL-8 | Visible-window normalisation | on/off | off | P1; rescale to the min/max of what's on screen | — |
| CTL-9 | Overtone guide count and opacity | 1 … 16, 0 … 1 | 8, 0.4 | P1; see §4 | ✅ |

**Analysis parameters** (recompute):

| ID | Parameter | Range | Default | Effect | Status |
|---|---|---|---|---|---|
| CTL-10 | Resolution (`gamma`) | 3 … 40 Hz | 11 | lower = sharper pitch, blurrier timing (P0) | ✅ |
| CTL-11 | Frequency range | low 20 … 500 Hz, high 1 … 11 kHz | 27.5 Hz … 8 kHz | P0; the initial view shows the vocal range E2 … C6 | ✅ |
| CTL-12 | Bins per semitone | 1 … 10 | 5 | vertical detail vs. memory/time (P1); memory scales with it (~6 MB per bin per semitone per 3 minutes); the pitch contour is self-checked at 1, 2 and 10 | ✅ |
| CTL-13 | Time step (hop) | 128 / 256 / 512 samples | 256 (11.6 ms) | horizontal detail vs. memory/time (P1) | — |
| CTL-14 | Channel | mix, left, right, side, centre | mix | P1; side-by-side buttons | ✅ |

**Workflow:**

| ID | | Status | Item |
|---|---|---|---|
| CTL-15 | P0 | ✅ | "Auto" buttons: re-run auto-calibration on the whole item, or on the visible region. |
| CTL-16 | P0 | ✅ | Reset to defaults. |
| CTL-17 | P0 | ✅ | Keyboard shortcuts for the most used ones: `[` `]` brightness, `-` `=` contrast, `,` `.` resolution. |
| CTL-18 | P1 | ◐ | Presets: "Vocal in mix" (centre channel, noise subtraction, 60 Hz … 5 kHz), "Solo vocal", "Fast runs" (gamma 25). User-saved presets: only "Save as default" / "Load default" for new items. |
| CTL-19 | P1 | ✅ | Settings remembered per item in the project, with global defaults for new items. Stored in project extended state under the item's GUID (item P_EXT strings are read through an API without a buffer size); loaded values are clamped. |
| CTL-20 | P1 | ✅ | Hover readout in the status line shows the cell's dB value, to help choose the floor. |
| CTL-21 | P2 | — | A/B toggle between two settings to compare. |

## 4. Overtones and pitch help

The hard part of transcribing from a spectrogram is telling fundamentals from overtones. With
vocals it's worse: formants often make the 2nd–4th harmonic brighter than the fundamental, so
octave errors are the most common mistake. Vocals are also rarely exactly on pitch and are full of
vibrato, scoops and slides, so a sung note has to be reduced to one semitone.

| ID | | Status | Item |
|---|---|---|---|
| PITCH-1 | P0 | ✅ | **Hover guides:** while hovering or drawing, marks at the overtones 1–8 of the pitch under the mouse (or the dragged note), numbered, 3 px thick. |
| PITCH-2 | P0 | ✅ | **Note overtone ghosts** (H): for each drawn note, faint outlines at its overtones. A bright line not covered by any note's overtone ghost is a missing note. |
| PITCH-3 | P0 | ✅ | **Subharmonic check:** dashed marks an octave, a 12th and two octaves below, labelled with their level relative to the pitch; orange when above −12 dB (the real note is probably lower). |
| PITCH-4 | P0 | ✅ | **Cents readout:** pitch snaps to the semitone; the status line shows the cents deviation of the strongest nearby peak. |
| PITCH-5 | P1 | ✅ | **Pitch contour** (P): harmonic-sum F0 over the vocal range with a Viterbi pass that penalises jumps (so a formant-boosted 3rd harmonic doesn't cause octave errors) and a voiced/unvoiced decision; drawn as a white line (`src/contour.cpp`, self-checked on synthetic sung notes). |
| PITCH-6 | P1 | ✅ | **Note pitch from the contour:** a drawn note takes the median sung pitch under it (ignoring the first 50 ms) when within 2 semitones; alt keeps the drawn row. Hovering a note shows its average sung offset in cents. |
| PITCH-7 | P1 | ✅ | **Suggest notes** (N): shown as dashed ghost notes; click one to accept it, A accepts all in view (into the active voice). From the note estimator (PITCH-12) by default, or from the lead contour: stable segments (±50 cents, ≥ 80 ms, vibrato smoothed over 150 ms), edges refined from the raw pitch and onsets. A suggestion is hidden by a drawn note in any voice within a semitone of it. |
| PITCH-8 | P1 | ✅ | **Explain-away view** (X): dims the energy on drawn notes (all voices) and their overtones. |
| PITCH-9 | P1 | — | Tuning reference (A4 = 440 Hz by default, adjustable, or auto-detected from the median cents offset of the contour). |
| PITCH-10 | P2 | — | Pitch bends from the contour for slides and vibrato (written as MIDI pitch bend, optional). |
| PITCH-11 | P2 | ◐ | Multiple pitch contours for harmonies (much harder; polyphonic F0). The note estimator (PITCH-12) suggests harmony notes; there is still only one contour line. |
| PITCH-12 | P1 | ✅ | **Note estimator** (`src/estimate.cpp`), ported from the `transcribe` project's NOTE-ESTIMATION.md: harmonic-template NMF on the NSGT (60 bins/octave, 1/3-semitone combs of 12 partials plus broadband atoms, KL updates, partial amplitudes adapted per 6 semitones from solo-ish frames), gated by basic-pitch's note posterior (> 0.12 within ±1 semitone), plus the enhanced summary autocorrelation (16 bands, ABS 0.25) as a union. Activity is a local pitch peak above 0.06 of the song's p99 and 0.15 of the frame maximum, with upper octaves dropped unless 2× the note below. Without a beat grid, runs start at full thresholds, continue at half, bridge 70 ms dropouts, split at basic-pitch onsets, and last ≥ 100 ms. Runs on the analysis channel; without the model it runs ungated (status line says why). Not ported: the front-end vote, octave doublings, product of experts (needs Cuesta) and voice assignment. Self-checked on synthetic chords and tones. |

## 5. Drawing and editing notes

| ID | | Status | Item |
|---|---|---|---|
| EDIT-1 | P0 | ✅ | Drag on empty space draws a note: pitch from the start row, start/end snapped (see §6). Ghost rectangle while dragging, minimum length 0.05 s. |
| EDIT-2 | P0 | ✅ | Click a note selects; cmd+click toggles; shift+drag marquee selects; cmd+A selects all; click empty space deselects. |
| EDIT-3 | P0 | ✅ | Drag a note body to move (time and pitch together); drag left/right edge to resize. DrawScore hit zones: rect inflated by 3 px, 6 px edge zones, notes under 12 px wide are body-only; 3 px threshold before a drag starts. (`src/editor.cpp`, self-checked.) |
| EDIT-4 | P0 | ✅ | Moves use original + delta (no float drift), snap relative to the anchor note, all selected notes move together. |
| EDIT-5 | P0 | ◐ | Delete/Backspace and right-click delete. Double-click edits the lyric instead of deleting. |
| EDIT-6 | P0 | ✅ | Each gesture is one REAPER undo point (`Undo_BeginBlock2`/`EndBlock2`); selection-only changes make none. |
| EDIT-7 | P1 | ✅ | Option/alt held = no snap while drawing, moving or resizing. |
| EDIT-8 | P1 | ✅ | Axis lock during a move once the drag passes 10 px (DrawScribe). Only while shift is held, so diagonal moves still work; releasing shift unlocks (self-checked). |
| EDIT-9 | P1 | — | Arrow keys nudge selection (left/right = one grid step, up/down = semitone, shift+up/down = octave). |
| EDIT-10 | P1 | ✅ | Copy, paste at edit cursor, duplicate (cmd+D), split at mouse, join selected notes. Keys: cmd+C/X/V, cmd+D, K (split), J (join). cmd+V pastes notes after a copy in this window and lyrics once other text has been copied (the system clipboard isn't overwritten). Split keeps the lyric on the first half; join takes the first note's pitch and joins the lyrics without hyphens (self-checked). Notes pasted past the MIDI item's end are hidden until INT-4. |
| EDIT-11 | P1 | ✅ | Draw a note and audition it immediately (see §7). |
| EDIT-12 | P0 | ✅ | Notes get a fixed velocity (96); no velocity editing. |
| EDIT-13 | P1 | ✅ | Multiple voices: up to four, each a MIDI item on its own track ("ReaTrace notes", "ReaTrace voice 2".."4") with its own colour and ReaSynth. Keys 1–4 or the panel pick the voice being edited; the others are shown dimmed and can be hidden. |
| EDIT-14 | P2 | — | "Draw along the line": drag along a spectral line and get a note whose start and end follow the energy (onset and decay detection). |

## 5a. Lyrics

Stored as MIDI lyric events (text event type 5, `MIDI_InsertTextSysexEvt`) at each note's start,
which is what REAPER's notation editor, its lyrics view, the web remote and the import/export track
lyrics actions use (`GetTrackMIDILyrics`/`SetTrackMIDILyrics`). Typing happens in a native edit
field, so non-ASCII letters, dead keys and paste work.

| ID | | Status | Item |
|---|---|---|---|
| LYR-1 | P0 | ◐ | Lyrics shown inside the note when it's at least 12 px tall, else just below it; no tooltip for truncated text. |
| LYR-2 | P0 | ✅ | Type a syllable: Enter (selected note, or the note under the mouse) or double-click starts editing; Tab/Space commits and moves to the next note, shift+Tab goes back, Enter finishes, Esc cancels. |
| LYR-3 | P0 | ✅ | Syllable conventions: typing `-` adds it and moves to the next note (`hel-` `lo`); `_` is stored as typed for melismas. |
| LYR-4 | P0 | ✅ | Lyric events move, resize and delete with their note, and are re-read when edited elsewhere in REAPER. |
| LYR-5 | P1 | ✅ | **Paste lyrics** (cmd+V): split into syllables (spaces, and after hyphens) and assigned in order to the selected notes, or from the selected note onwards. |
| LYR-6 | P1 | — | Automatic hyphenation suggestion for pasting plain words (dictionary-based, English first). |
| LYR-7 | P1 | ✅ | Lyrics lane: a strip under the notes showing the lyric line for the visible range, so the text can be read as a sentence while placing notes. Each syllable starts at its note's start (with a tick in the voice colour) and is cut off at the next one; selected notes' syllables are white; clicking one edits it. Active voice only. |
| LYR-8 | P2 | — | Lyrics without notes yet (e.g. spoken passages) placed on the time ruler and later attached to notes. |

## 6. Time, grid and quantisation

Recordings are usually free tempo, so the work is in two phases: first transcribe the performed
timing, then (optionally) fit REAPER's tempo map to the performance and quantise. REAPER already
has the tempo-mapping tools: tempo/time-signature markers with gradual changes, "create measure
from time selection" (detects the tempo of a selected bar), tap tempo, transient guides, and SWS's
tempo tools and markers-to-tempo conversion. We display the tempo map and leave mapping to those.

| ID | | Status | Item |
|---|---|---|---|
| TIME-1 | P0 | ◐ | Grid from REAPER's tempo map, shaded by bar/beat/subdivision, updating live. The subdivision is chosen by zoom rather than from REAPER's grid setting. |
| TIME-2 | P0 | ◐ | Snap modes off, onsets, grid (G). Default is onsets rather than off, since it only snaps within 30 ms; grid mode doesn't follow REAPER's snap setting. |
| TIME-3 | P0 | ✅ | Note start/end snap to nearby onsets and pitch-contour note changes within 30 ms (alt disables). |
| TIME-4 | P1 | ✅ | **Onset ticks:** detected onsets (spectral flux above mean + 1.5·std, local maxima, ≥ 50 ms apart) shown as orange ticks on the ruler. |
| TIME-5 | P1 | ✗ | **Tempo-map shortcuts:** built (selection = 1 bar/beat, tempo marker at cursor), then removed since REAPER's own tools cover tempo mapping. The ruler's shift+drag time selection stays. |
| TIME-6 | P1 | ◐ | **Zoom-aware snap:** the finest of 1/32, 1/16, 1/8, 1/4 (or bars) whose step is ≥ 10 px; no triplets. |
| TIME-7 | P1 | ◐ | Quantise selected notes (Q) to the grid with strength 0–100%; starts and ends together, not separately. |
| TIME-8 | P1 | ◐ | New notes items use the time timebase, so tempo edits don't move the item. The notes inside are stored in beats, so they still follow tempo map changes. |
| TIME-9 | P2 | — | Keep the performed timing and offer a quantised copy on a separate item. |
| TIME-10 | P2 | — | Triplet/tuplet aware snapping in the same bar. |

## 7. Playback and listening

| ID | | Status | Item |
|---|---|---|---|
| PLAY-1 | P0 | ✅ | Space = REAPER play/stop (via REAPER's shortcuts); play cursor and edit cursor drawn in the window; click the ruler to move the edit cursor. |
| PLAY-2 | P0 | ✅ | Notes play through the voice track's synth (ReaSynth added on setup) together with the audio. |
| PLAY-3 | P1 | ✅ | Audition on click: piano keys, clicked notes, drawn notes and pitch changes while dragging play through the voice track as a track preview of a generated one-note MIDI file (written safely to REAPER's resource folder). Previews play to their end, lasting longer at slower playrates, so notes don't hang. |
| PLAY-4 | P1 | ✅ | Loop: shift+drag in the ruler sets REAPER's time selection and loop points (whether or not REAPER links them). R loops the selected notes' range (or the time selection): sets it, turns on repeat and plays from its start; R again stops. |
| PLAY-5 | P1 | ✅ | Balance controls: audio vs. notes volume, solo audio, solo notes, quick A/B. Panel: "Listen to" Both / Audio / Notes (mutes the audio track or the voice tracks) and a notes volume slider (the voice tracks' faders, −30 … +6 dB); B swaps audio/notes. Uses track mutes and volume rather than item volume, which could reach the spectrogram's audio reader; the panel reads them back, so changes made in REAPER show. Muting the audio track also mutes anything else on it. |
| PLAY-6 | P1 | ✅ | Slow playback: 100/75/50% buttons set the master playrate and turn on preserve pitch (action 40671, assumed). |
| PLAY-7 | P2 | — | Scrub with the mouse over the ruler (REAPER's scrub, or DrawScore's spectral-freeze idea, unproven). |
| PLAY-8 | P1 | — | Reference tone while holding a key on the keyboard, to compare against the sung pitch. |
| PLAY-9 | P2 | — | Filtered listening: play only the frequency band around the selected note's pitch and its overtones (isolates the vocal in a mix). |

## 8. REAPER integration and robustness

| ID | | Status | Item |
|---|---|---|---|
| INT-1 | P0 | ✅ | Setup action creates the "ReaTrace notes" track (with ReaSynth) and a MIDI item covering the audio item; reopening finds the existing ones. |
| INT-2 | P0 | ✅ | External changes (MIDI editor edits, undo/redo, item moves, deleted items) are picked up: notes and lyrics re-read when the take's hash changes (`MIDI_GetHash`); the window closes if the audio item disappears. |
| INT-3 | P0 | ✅ | Audio item position, take offset and playrate respected when mapping spectrogram time to project time. |
| INT-4 | P1 | — | MIDI item extended to cover notes drawn past its end. |
| INT-5 | P1 | — | Several audio items on a track (comping, edits): show all of them, each with its own cached analysis. |
| INT-6 | P2 | ✅ | Store per-item settings in the project (done early, see §3a). |

## 9. Performance

Measured on Apple Silicon for 3 minutes of audio:

- Spectrogram: ~0.2 s; pitch contour ~0.23 s; onsets and segments ~0.05 s. All in the background.
- Note estimator: basic-pitch ~1.2–1.8 s (CoreML, CPU), adapted NMF ~1.3–2.3 s, SACF ~1.2 s (all cores); also in the background, after the spectrogram is shown.
- Recolouring after a display setting change: ~40–60 ms.
- Memory: ~30 MB per analysed channel (float magnitudes), cached per channel while the window is open.
- Redraw: the spectrogram layer is re-rendered only when the view or values change; at Retina
  resolution this is four times the pixels and hasn't been measured.

---

## Milestones

1. ✅ **P0 core:** window, layout, navigation, spectrogram view with the settings panel, grid from
   the tempo map, draw/select/move/resize/delete, undo, playback cursors, sync with the MIDI take.
2. ✅ **Pitch aids and lyrics:** hover overtone guides, note overtone ghosts, subharmonic check,
   cents readout, lyric entry and display, audition on click.
3. ✅ **Vocal speed-ups:** pitch contour, note pitch from contour, suggested notes, snap to contour
   changes/onsets, channel analysis (incl. centre), slow playback presets, paste lyrics.
4. ✅ **Polish:** quantise, voices, explain-away view, background analysis, presets and per-item
   settings, docking, Retina. Tempo-map shortcuts were built and then dropped (REAPER covers them).
5. ◐ **Editing, listening and lyrics polish** (leftover P1s). Done: copy/paste/duplicate/split/join
   (EDIT-10), axis lock (EDIT-8), audio/notes balance and solo (PLAY-5), lyrics lane (LYR-7), and
   alongside them theme colours (WIN-6), arrange view link (NAV-5), overview (NAV-7), bass analysis
   (SPEC-5), bins per semitone (CTL-12), looping (PLAY-4). Left: arrow-key nudge (EDIT-9), follow
   playback (NAV-4), reference tone (PLAY-8), MIDI item auto-extend (INT-4), several audio items
   per track (INT-5), tuning reference (PITCH-9).
6. — **Experiments:** pitch bends from the contour (PITCH-10), harmony contours (PITCH-11),
   harmonic/percussive separation (SPEC-9), filtered listening (PLAY-9), automatic hyphenation
   (LYR-6).
