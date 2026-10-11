# Reascribe piano roll: requirements

A custom REAPER window for transcribing audio by drawing notes on a spectrogram.
Priorities: **P0** = first usable version, **P1** = makes transcription fast, **P2** = later / experimental.

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
  Notes must always stay readable over a bright spectrogram.
- **Reuse REAPER** for transport, tempo map, grid, undo, synth and playback rate instead of rebuilding them.

---

## 1. Window and layout

| | Item |
|---|---|
| P0 | Resizable window, opened by an action on the selected audio item (or the selected Reascribe MIDI item). |
| P0 | Layout: piano keyboard on the left, time ruler on top (bars/beats from the tempo map, plus seconds), spectrogram+notes in the middle, status line (pitch name, Hz, cents off, time, bar.beat under the mouse). |
| P0 | Window receives keyboard shortcuts (needs REAPER's `accelerator` hook, since REAPER grabs keys first). Unhandled keys like Space pass to REAPER. |
| P1 | Dockable in REAPER's docker (`DockWindowAddEx`), position and size remembered. |
| P1 | Retina: render the spectrogram and notes at 2× backing resolution. |
| P1 | Colours follow the REAPER theme (background, grid, note colours) or a built-in dark theme. |
| P2 | Optional waveform strip under the ruler, for spotting onsets. |
| P2 | Windows support (dialog template and DIB section instead of SWELL-only calls). |

## 2. Navigation

| | Item |
|---|---|
| P0 | Horizontal zoom (wheel / pinch) around the mouse, horizontal scroll (shift+wheel / trackpad). |
| P0 | Vertical zoom (alt/cmd+wheel / pinch) and scroll; DrawScore-style directional pinch (horizontal pinch zooms time, vertical zooms pitch). |
| P0 | Zoom to fit the item; zoom to selection. |
| P1 | Follow playback (page or continuous scroll), toggleable. |
| P1 | Keep the view in sync with REAPER's arrange view (optional link). |
| P1 | Trackpad pan with inertia (DrawScore: ×0.95 per frame, stop below 0.5 px/s). |
| P2 | Mini overview of the whole item with the visible range. |

## 3. Spectrogram display

| | Item |
|---|---|
| P0 | NSGT spectrogram (done in `src/nsgt.cpp`), computed once per item and cached, mapped to the view with max-pooling. |
| P0 | Pitch axis aligned so each note row is centred on its pitch's frequency (DrawScore: notes span pitch ±0.5 semitone, interpolated over bin centres). |
| P0 | Auto-calibrated contrast (done) as the starting point; every parameter is adjustable (see §3a). |
| P1 | Recompute in the background with progress, so long items don't freeze REAPER. Cache keyed by source file + take offset/playrate + analysis parameters, optionally saved to disk. |
| P1 | Optional second "bass" analysis with longer windows, blended below a crossover pitch. |
| P1 | Display filters: peaks only, noise subtraction (done), visible-window normalisation (DrawScribe). |
| P1 | Recompute automatically when the audio item's source, offset, length or playrate changes. |
| P1 | Channel choice: mix, left, right, mid, side. Lead vocals are usually panned centre, so "mid" helps in mixes, and mid minus side suppresses wide instruments. |
| P2 | Harmonic/percussive separation (median filtering) to show tonal content without drum smear. |
| P2 | Spectral whitening / per-bin normalisation for quiet high parts. |

## 3a. Spectrogram controls

A settings panel (collapsible side panel or popover in the window) with a slider plus a number
field per parameter. Display changes apply instantly from the cached magnitudes; analysis changes
recompute (in the background once that exists) and show a progress indicator.

**Display parameters** (instant, P0 unless noted):

| Parameter | Range | Default | Effect |
|---|---|---|---|
| dB floor | −120 … −20 dB | auto (p30) | level that maps to black/transparent |
| Contrast | 0.1 … 3 | auto (from p95) | spread of the colour range |
| Brightness | −1 … 1 | auto (−0.10) | shifts everything up or down |
| Colour curve (bias) | −2 … 2 | −0.8 | gamma of the colormap; negative darkens quiet parts |
| Colormap | magma, inferno, viridis, plasma, cividis, grey | magma | P1 |
| Peaks only | on/off, dim factor 0 … 1 | off, 0.15 | keep per-frame frequency peaks, dim the rest |
| Noise subtraction | on/off, strength 0 … 3, noise percentile 1 … 50% | off, 1.5, 10% | subtract the quiet-frame spectrum |
| Visible-window normalisation | on/off | off | P1; rescale to the min/max of what's on screen |
| Overtone guide count and opacity | 1 … 16, 0 … 1 | 8, 0.4 | P1; see §4 |

**Analysis parameters** (recompute):

| Parameter | Range | Default | Effect |
|---|---|---|---|
| Resolution (`gamma`) | ~3 … 40 Hz | 11 | lower = sharper pitch, blurrier timing (P0) |
| Frequency range | 20 Hz … 11 kHz (low, high) | 27.5 Hz … 8 kHz | P0; also sets the pitch range the view can scroll to. The initial view shows the vocal range E2 … C6. |
| Bins per semitone | 1 … 10 | 5 | vertical detail vs. memory/time (P1) |
| Time step (hop) | 128 / 256 / 512 samples | 256 (11.6 ms) | horizontal detail vs. memory/time (P1) |
| Channel | mix, left, right, mid, side | mix | P1 |

**Workflow:**

| | Item |
|---|---|
| P0 | "Auto" button: re-run auto-calibration on the whole item, or on the visible region. |
| P0 | Reset to defaults. |
| P0 | Keyboard shortcuts for the most used ones (brightness, contrast, resolution), so they can be tweaked without the panel. |
| P1 | Presets: e.g. "vocals in a mix" (mid channel, noise subtraction, 80 Hz … 5 kHz), "solo vocal", "fast runs" (high gamma), plus user-saved presets. |
| P1 | Settings remembered per item in the project (item extended state), with global defaults for new items. |
| P1 | Hover readout in the status line shows the cell's dB value, to help choose the floor. |
| P2 | A/B toggle between two settings to compare. |

## 4. Overtones and pitch help

The hard part of transcribing from a spectrogram is telling fundamentals from overtones. With
vocals it's worse: formants often make the 2nd–4th harmonic brighter than the fundamental, so
octave errors are the most common mistake. Vocals are also rarely exactly on pitch and are full of
vibrato, scoops and slides, so a sung note has to be reduced to one semitone.

| | Item |
|---|---|
| P0 | **Hover guides:** while hovering or drawing, show faint marks at the overtones of the pitch under the mouse (f, 2f, 3f … up to ~8f: +12, +19, +24, +28, +31, +34, +36 semitones), so you can check that the stack of lines belongs to one note. |
| P0 | **Note overtone ghosts:** for each drawn note, optionally draw its overtones as faint outlines. A bright line not covered by any note's overtone ghost is a missing note. |
| P0 | **Subharmonic check:** when drawing a note, mark the pitches whose overtones also fit (an octave or a 12th below), to catch octave errors. |
| P0 | **Cents readout:** pitch snaps to the semitone; the status line shows the cents deviation of the strongest nearby peak. |
| P1 | **Pitch contour:** an F0 track for the lead vocal (harmonic-sum / HPS estimate from Specter, restricted to the vocal range, with a voiced/unvoiced decision) drawn as a thin line over the spectrogram. |
| P1 | **Note pitch from the contour:** a drawn note's pitch is the median of the contour under it (ignoring the first ~50 ms of scoop), rounded to the semitone; shows the note's average cents offset. |
| P1 | **Suggest notes:** split the contour into stable segments (pitch stays within ±50 cents for ≥ 80 ms; vibrato averaged out) and show them as ghost notes that can be accepted one by one or all at once. |
| P1 | **Explain-away view:** dim the spectrogram energy that is covered by the overtones of drawn notes, so the remaining bright spots are what still needs transcribing. |
| P1 | Tuning reference (A4 = 440 Hz by default, adjustable, or auto-detected from the median cents offset of the contour). |
| P2 | Pitch bends from the contour for slides and vibrato (written as MIDI pitch bend, optional). |
| P2 | Multiple pitch contours for harmonies (much harder; polyphonic F0). |

## 5. Drawing and editing notes

| | Item |
|---|---|
| P0 | Drag on empty space draws a note: pitch from the start row, start/end snapped to the grid. Ghost rectangle while dragging, minimum length one grid step (or 0.05 s without snap). |
| P0 | Click a note selects; cmd+click toggles; shift+drag marquee selects; cmd+A selects all; click empty space deselects. |
| P0 | Drag a note body to move (time and pitch together); drag left/right edge to resize. DrawScore hit zones: rect inflated by 3 px, 6 px edge zones, notes under 12 px wide are body-only; 3 px threshold before a drag starts. |
| P0 | Moves use original + delta (no float drift), snap relative to the anchor note, all selected notes move together. |
| P0 | Delete/Backspace deletes; double-click a note deletes it (or right-click, configurable). |
| P0 | Each gesture is one REAPER undo point (`Undo_BeginBlock2`/`EndBlock2`). |
| P1 | Option/alt held = no snap while drawing, moving or resizing. |
| P1 | Axis lock during a move once the drag passes 10 px (DrawScribe). |
| P1 | Arrow keys nudge selection (left/right = one grid step, up/down = semitone, shift+up/down = octave). |
| P1 | Copy, paste at edit cursor, duplicate (cmd+D), split at mouse, join selected notes. |
| P1 | Draw a note and audition it immediately (see §7). |
| P0 | Notes get a fixed velocity (setting, default 96); no velocity editing. |
| P1 | Multiple voices: one MIDI item per voice/track (lead, harmony 1, harmony 2, backing), each with its own colour. The active voice is edited; others are shown dimmed and can be hidden. |
| P2 | "Draw along the line": drag along a spectral line and get a note whose start and end follow the energy (onset and decay detection). |

## 5a. Lyrics

Stored as MIDI lyric events (text event type 5, `MIDI_InsertTextSysexEvt`) at each note's start,
which is what REAPER's notation editor, its lyrics view, the web remote and the import/export track
lyrics actions use (`GetTrackMIDILyrics`/`SetTrackMIDILyrics`).

| | Item |
|---|---|
| P0 | Lyrics shown inside or just below each note, readable at normal zoom, truncated with a tooltip when the note is too short. |
| P0 | Type a syllable on the selected note: Enter or double-click starts editing, Tab/Space commits and moves to the next note, shift+Tab goes back, Esc cancels. |
| P0 | Syllable conventions: a trailing `-` joins to the next syllable (`hel-` `lo`), and an empty syllable or `_` on a following note marks a melisma (one syllable held over several notes). |
| P0 | Lyric events move, resize and delete with their note, and are re-read when edited elsewhere in REAPER. |
| P1 | **Paste lyrics:** paste a block of text, split it into syllables (spaces and hyphens), and assign them in order to the selected notes or from the selected note onwards. |
| P1 | Automatic hyphenation suggestion for pasting plain words (dictionary-based, English first). |
| P1 | Lyrics lane: a strip under the notes showing the lyric line for the visible range, so the text can be read as a sentence while placing notes. |
| P2 | Lyrics without notes yet (e.g. spoken passages) placed on the time ruler and later attached to notes. |

## 6. Time, grid and quantisation

| | Item |
|---|---|
Recordings are usually free tempo, so the work is in two phases: first transcribe the performed
timing, then (optionally) fit REAPER's tempo map to the performance and quantise. REAPER already
has the tempo-mapping tools: tempo/time-signature markers with gradual changes, "create measure
from time selection" (detects the tempo of a selected bar), tap tempo, transient guides, and SWS's
tempo tools and markers-to-tempo conversion. We display the tempo map and hand off to those.

| | Item |
|---|---|
| P0 | Grid from REAPER's tempo map and grid setting; grid lines shaded by bar/beat/subdivision; updates live when the tempo map changes. |
| P0 | Snap off by default for free-tempo work; snap modes: off, grid, onsets. Follows REAPER's snap setting when set to grid. |
| P0 | Note start/end snap to nearby pitch-contour changes or onsets when within ~30 ms (with the modifier to disable). |
| P1 | **Onset ticks:** detected onsets (spectral flux, Specter's threshold mean + 1.5·std, plus contour pitch changes, since sung onsets are often soft) shown as faint ticks on the ruler. |
| P1 | **Tempo-map shortcuts:** select a bar in our ruler and run REAPER's "create measure from time selection"; insert a tempo marker at the edit cursor; onset ticks help place bar lines. |
| P1 | **Zoom-aware snap** (DrawScore default) once a tempo map exists: the finest of 1/32, 1/32T, 1/16, 1/8T, 1/8, 1/4, bar whose step is ≥ 10 px on screen. |
| P1 | Quantise selected notes: starts, ends or both, to grid, with strength (0–100%). |
| P1 | Notes keep their real-time positions when the tempo map changes (MIDI item timebase set to time), with an explicit "quantise" step afterwards. |
| P2 | Keep the performed timing and offer a quantised copy on a separate item. |
| P2 | Triplet/tuplet aware snapping in the same bar. |

## 7. Playback and listening

| | Item |
|---|---|
| P0 | Space = REAPER play/stop; play cursor and edit cursor drawn in the window; click the ruler to move the edit cursor. |
| P0 | Notes play through the MIDI track's synth (ReaSynth added on setup) together with the audio. |
| P1 | Audition on click: clicking a key on the keyboard or drawing/selecting a note plays it briefly (`StuffMIDIMessage` to the notes track's input, or a preview). |
| P1 | Loop: time selection in the ruler sets REAPER's loop; play selection only. |
| P1 | Balance controls: audio vs. notes volume, solo audio, solo notes, quick A/B. |
| P1 | Slow playback (REAPER playrate with preserve pitch), e.g. 50%/75% presets. |
| P2 | Scrub with the mouse over the ruler (REAPER's scrub, or DrawScore's spectral-freeze idea, unproven). |
| P1 | Reference tone while holding a key on the keyboard, to compare against the sung pitch. |
| P2 | Filtered listening: play only the frequency band around the selected note's pitch and its overtones (isolates the vocal in a mix). |

## 8. REAPER integration and robustness

| | Item |
|---|---|
| P0 | Setup action creates the "Reascribe notes" track (with ReaSynth) and a MIDI item covering the audio item; reopening finds the existing ones. |
| P0 | External changes (MIDI editor edits, undo/redo, item moves, deleted items) are picked up: re-read notes when the take's hash changes (`MIDI_GetHash`); close or blank the view if the item disappears. |
| P0 | Audio item position, take offset and playrate respected when mapping spectrogram time to project time. |
| P1 | MIDI item extended to cover notes drawn past its end. |
| P1 | Several audio items on a track (comping, edits): show all of them, each with its own cached analysis. |
| P2 | Store per-item settings (gamma, contrast, filters, voices) in the project via take/item extended state. |

## 9. Performance targets

- Analysis: 3 min of audio in under 0.5 s on Apple Silicon (currently ~0.2 s single-threaded), or in the background.
- Redraw on scroll/zoom: under 16 ms for a full-screen window, using the quantised value cache.
- Idle cost: no redraws when nothing changes; cursor-only redraws during playback.
- Memory: ~30 MB per 3 min of audio for the magnitude cache (float); switch to 8-bit dB if hour-long items matter.

---

## Suggested milestones

1. **P0 core:** window, layout, navigation, spectrogram view with the settings panel (display
   parameters, resolution, frequency range, auto, reset), grid from the tempo map, draw/select/move/
   resize/delete, undo, playback cursors, sync with the MIDI take.
2. **Pitch aids and lyrics:** hover overtone guides, note overtone ghosts, subharmonic check, cents
   readout, lyric entry and display, audition on click.
3. **Vocal speed-ups:** pitch contour, note pitch from contour, suggested notes, snap to contour
   changes/onsets, mid-channel analysis, slow playback presets, paste lyrics.
4. **Tempo and polish:** tempo-map shortcuts, quantise, voices, explain-away view, background
   analysis, presets and per-item settings, docking, Retina.
5. **Experiments:** pitch bends from the contour, harmony contours, harmonic/percussive separation,
   filtered listening, automatic hyphenation.
