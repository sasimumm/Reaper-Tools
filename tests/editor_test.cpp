// Self-check for note editing gestures: run `build/editor_test`.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "index.h"
#include "notes/editor.h"

// Equal but for rounding.
static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

int main() {
    ViewMap v; // 100 px/s, 12 px/semitone, pitch 84.5 at y=0
    NoteEditor ed;
    ed.snap = [](double t) { return round(t * 4) / 4; }; // quarter-second grid

    // Drawing on empty space creates a snapped note at the row's pitch.
    const double y60 = v.y(60);
    ed.mouseDown(v, v.x(1.02), y60, 0);
    ed.mouseMove(v, v.x(1.4), y60, 0);
    assert(ed.drawing() == 0);
    assert(ed.mouseUp(v, v.x(1.9), y60, 0));
    assert(ed.notes.size() == 1 && ed.notes[0].pitch == 60 && ed.notes[0].sel &&
           ed.notes[0].idx == -1);
    assert(near(ed.notes[0].start, 1.0) && near(ed.notes[0].end, 2.0));

    // Just above a note (but within a row's height) draws rather than grabbing the note.
    {
        NoteEditor e;
        e.notes.push_back({1.0, 2.0, 60, false, 3});
        int hit = -1;
        assert(e.hitTest(v, v.x(1.01), v.y(60.5) - 1, &hit) == NoteEditor::NONE);
        assert(e.hitTest(v, v.x(1.01), v.y(60.5) + 1, &hit) == NoteEditor::LEFT && hit == 0);
    }

    // A click on empty space without dragging deselects and draws nothing.
    ed.mouseDown(v, v.x(5), y60, 0);
    assert(ed.mouseUp(v, v.x(5) + 1, y60, 0));
    assert(ed.notes.size() == 1 && !ed.notes[0].sel);

    // With clickPlaces, the click places a note as long as the last one drawn, start snapped.
    ed.clickPlaces = true;
    ed.mouseDown(v, v.x(5.1), v.y(62), 0);
    assert(ed.mouseUp(v, v.x(5.1), v.y(62), 0));
    assert(ed.notes.size() == 2 && ed.notes[1].sel && ed.notes[1].pitch == 62);
    assert(near(ed.notes[1].start, 5.0) && near(ed.notes[1].end, 6.0));
    ed.notes.pop_back();
    ed.notes[0].sel = false;
    ed.clickPlaces = false;

    // Dragging the body moves time (snapped via the note's start) and pitch.
    ed.notes.push_back({3.0, 3.5, 64, false, 7});
    ed.mouseDown(v, v.x(1.5), y60, 0);
    ed.mouseUp(v, v.x(2.05), v.y(62), 0);
    assert(near(ed.notes[0].start, 1.5) && near(ed.notes[0].end, 2.5) && ed.notes[0].pitch == 62);
    assert(near(ed.notes[1].start, 3.0) && ed.notes[1].pitch == 64); // unselected, untouched

    // Alt disables snapping.
    ed.mouseDown(v, v.x(2.0), v.y(62), 0);
    ed.mouseUp(v, v.x(2.13), v.y(62), MOD_ALT);
    assert(near(ed.notes[0].start, 1.63));

    // Shift locks a move to the axis it mostly went along, once past 10 px; releasing shift
    // unlocks.
    ed.notes = {{1.0, 2.0, 60, true, -1}};
    ed.snap = [](double t) { return t; };
    ed.mouseDown(v, v.x(1.5), v.y(60), 0);
    ed.mouseMove(v, v.x(1.5) + 30, v.y(60) - 8, MOD_SHIFT); // mostly right: time only
    ed.mouseMove(v, v.x(1.5) + 30, v.y(63), MOD_SHIFT);
    assert(near(ed.notes[0].start, 1.3) && ed.notes[0].pitch == 60);
    ed.mouseUp(v, v.x(1.5) + 30, v.y(63), 0); // shift released: free again
    assert(near(ed.notes[0].start, 1.3) && ed.notes[0].pitch == 63);
    ed.mouseDown(v, v.x(1.8), v.y(63), 0);
    ed.mouseMove(v, v.x(1.8) + 4, v.y(65), MOD_SHIFT); // mostly up: pitch only
    ed.mouseUp(v, v.x(2.8), v.y(65), MOD_SHIFT);
    assert(near(ed.notes[0].start, 1.3) && ed.notes[0].pitch == 65);
    ed.snap = [](double t) { return round(t * 4) / 4; };

    // Dragging the right edge resizes; the end can't pass the start.
    ed.notes[0] = {1.0, 2.0, 62, true, -1};
    ed.mouseDown(v, v.x(2.0) - 2, v.y(62), 0);
    ed.mouseUp(v, v.x(2.6), v.y(62), 0);
    assert(near(ed.notes[0].start, 1.0) && near(ed.notes[0].end, 2.5));
    ed.mouseDown(v, v.x(2.5) - 2, v.y(62), 0);
    ed.mouseUp(v, v.x(0.0), v.y(62), 0);
    assert(near(ed.notes[0].end, 1.0 + ed.minLen));

    // Cmd+click toggles; shift+drag marquee selects what it touches.
    ed.notes = {{1, 2, 60, false, 0}, {3, 4, 60, false, 1}, {5, 6, 72, false, 2}};
    ed.mouseDown(v, v.x(1.5), v.y(60), MOD_CMD);
    ed.mouseUp(v, v.x(1.5), v.y(60), MOD_CMD);
    assert(ed.notes[0].sel && !ed.notes[1].sel);
    ed.mouseDown(v, v.x(2.5), v.y(58), MOD_SHIFT);
    ed.mouseUp(v, v.x(4.5), v.y(61), MOD_SHIFT);
    assert(!ed.notes[0].sel && ed.notes[1].sel && !ed.notes[2].sel);

    // Cmd+drag moves copies of the selection and the pressed note; the originals stay.
    {
        NoteEditor e;
        e.notes = {{1, 2, 60, true, 0, "la"}, {3, 4, 62, false, 1}, {5, 6, 64, false, 2}};
        e.mouseDown(v, v.x(3.5), v.y(62), MOD_CMD);
        e.mouseMove(v, v.x(5.5), v.y(62), MOD_CMD);
        e.cancel();
        assert(e.notes.size() == 3 && e.notes[0].sel && !e.notes[1].sel);
        e.mouseDown(v, v.x(3.5), v.y(62), MOD_CMD);
        assert(e.mouseUp(v, v.x(5.5), v.y(64), MOD_CMD));
        assert(e.notes.size() == 5 && !e.notes[0].sel && !e.notes[1].sel);
        assert(near(e.notes[0].start, 1) && near(e.notes[1].start, 3) && e.notes[1].idx == 1);
        const Note &a = e.notes[3], &b = e.notes[4];
        assert(a.sel && near(a.start, 3) && a.pitch == 62 && a.idx == -1 && a.lyric == "la");
        assert(b.sel && near(b.start, 5) && b.pitch == 64 && b.idx == -1);
    }

    // Copies move by whole clone steps in quarter notes (tempo 120: 2 per second, then 60 from
    // 4 s), not by the snapped pressed note, so each lands on the same beat it had.
    {
        NoteEditor e;
        e.snap = [](double t) { return t + 0.07; }; // an onset nearby: not used for copies
        e.toQN = [](double t) { return t <= 4 ? t * 2 : 8 + (t - 4); };
        e.fromQN = [](double q) { return q <= 8 ? q / 2 : 4 + (q - 8); };
        e.cloneStep = [](double dq) { return round(dq); };
        e.notes = {{1, 1.5, 60, true, 0}, {2, 2.5, 62, true, 1}};
        e.mouseDown(v, v.x(1.2), v.y(60), MOD_CMD);
        assert(e.mouseUp(v, v.x(3.25), v.y(60), MOD_CMD)); // 2.05 s: 4.1 qn, rounded to 4
        assert(e.notes.size() == 4 && near(e.notes[0].start, 1) && near(e.notes[1].start, 2));
        assert(near(e.notes[2].start, 3) && near(e.notes[2].end, 3.5)); // qn 2..3 -> 6..7
        assert(near(e.notes[3].start, 4) && near(e.notes[3].end, 5));   // qn 4..5 -> 8..9
    }

    // Pasting puts the earliest note at `at` and the rest the same beats after it (tempo 120
    // until 4 s, then 60).
    {
        NoteEditor e;
        e.toQN = [](double t) { return t <= 4 ? t * 2 : 8 + (t - 4); };
        e.fromQN = [](double q) { return q <= 8 ? q / 2 : 4 + (q - 8); };
        const std::vector<Note> clip = {{1.5, 2, 62, false, -1, "b"}, {1, 1.5, 60, false, -1, "a"}};
        e.paste(clip, 3.5); // qn 7: +5 qn
        assert(e.notes.size() == 2 && e.notes[1].sel && e.notes[1].lyric == "a");
        assert(near(e.notes[1].start, 3.5) && near(e.notes[1].end, 4)); // qn 2..3 -> 7..8
        assert(near(e.notes[0].start, 4) && near(e.notes[0].end, 5));   // qn 3..4 -> 8..9
    }

    // Resizing stops at the next or previous note of the same pitch (another pitch doesn't).
    {
        NoteEditor e;
        e.notes = {{1, 2, 60, false, 0}, {3, 4, 60, false, 1}, {2.5, 2.8, 62, false, 2}};
        e.mouseDown(v, v.x(2) - 1, v.y(60), 0); // the first note's right edge
        assert(e.mouseUp(v, v.x(3.6), v.y(60), 0));
        assert(near(e.notes[0].end, 3));        // stopped at the second note
        e.mouseDown(v, v.x(3) + 1, v.y(60), 0); // the second note's left edge
        e.mouseUp(v, v.x(1.2), v.y(60), 0);
        assert(near(e.notes[1].start, 3)); // the first note now ends there
    }

    // Shift+Delete clears the selected notes' lyrics, nothing else.
    {
        NoteEditor e;
        e.notes = {{1, 2, 60, true, 0, "la"}, {3, 4, 62, false, 1, "lo"}, {5, 6, 64, true, 2}};
        assert(e.clearLyrics() && e.notes.size() == 3 && e.notes[0].lyric.empty());
        assert(e.notes[1].lyric == "lo" && !e.clearLyrics());
    }

    // Moving several selected notes keeps their spacing; Delete records take indices.
    ed.selectAll(true);
    ed.mouseDown(v, v.x(3.5), v.y(60), 0);
    ed.mouseUp(v, v.x(4.0), v.y(60), 0);
    assert(near(ed.notes[0].start, 1.5) && near(ed.notes[2].start, 5.5));
    ed.notes[2].sel = ed.notes[0].sel = false;
    assert(ed.deleteSelected() && ed.notes.size() == 2 && ed.deleted.size() == 1 &&
           ed.deleted[0] == 1);

    // Cancel restores the notes from before the gesture.
    ed.mouseDown(v, v.x(1.75), v.y(60), 0);
    ed.mouseMove(v, v.x(3), v.y(70), 0);
    ed.cancel();
    assert(near(ed.notes[0].start, 1.5) && ed.notes[0].pitch == 60);

    // Narrow notes have no resize edges.
    ed.notes = {{1, 1.05, 60, false, 0}};
    int i = 0;
    assert(ed.hitTest(v, v.x(1.0), v.y(60), &i) == NoteEditor::BODY);

    // Pasted lyrics: one syllable per note, hyphens stay with their syllable.
    std::vector<std::string> syl = splitSyllables("Hel-lo  dar-k-ness\nmy _ old\tfriend- -");
    const std::array<const char *, 9> want = {"Hel-", "lo", "dar-", "k-",     "ness",
                                              "my",   "_",  "old",  "friend-"};
    assert(syl.size() == 9);
    for (int k = 0; k < 9; k++) {
        assert(syl[idx(k)] == want[idx(k)]);
    }

    // Copy/paste at a time, duplicate after the selection, split, join (lyrics joined without
    // hyphens).
    {
        NoteEditor e;
        e.notes = {{1, 2, 60, true, 0, "hel-", 3}, {2, 3, 62, true, 1, "lo"}, {5, 6, 64, false, 2}};
        const std::vector<Note> clip = e.copySelected();
        e.paste(clip, 10);
        assert(e.notes.size() == 5 && near(e.notes[3].start, 10) && near(e.notes[4].end, 12) &&
               e.notes[3].idx == -1);
        assert(e.notes[3].lyric == "hel-" && e.notes[3].lyricIdx == -1 && e.notes[3].sel &&
               !e.notes[0].sel);
        assert(e.duplicateSelected() && e.notes.size() == 7 && near(e.notes[5].start, 12) &&
               near(e.notes[6].end, 14));

        // nudge: selected notes only, all or nothing
        e.notes = {{1, 2, 60, true, 0, "la"}, {3, 4, 120, true, 1}, {5, 6, 64, false, 2}};
        auto later = [](double t) { return t + 0.5; };
        assert(e.nudgeSelected(later, 1) && near(e.notes[0].start, 1.5) &&
               near(e.notes[0].end, 2.5) && e.notes[0].pitch == 61 && e.notes[0].lyric == "la");
        assert(e.notes[1].pitch == 121 && near(e.notes[2].start, 5) && e.notes[2].pitch == 64);
        assert(!e.nudgeSelected([](double t) { return t; }, 12) && e.notes[1].pitch == 121 &&
               e.notes[0].pitch == 61); // 133 is out of range: nothing moves
        assert(!e.nudgeSelected([](double t) { return t - 2; }, 0) && near(e.notes[0].start, 1.5));
        // a move that would reverse a note (its end before its start) moves nothing
        assert(!e.nudgeSelected([](double t) { return t < 1.6 ? t + 2 : t; }, 0) &&
               near(e.notes[0].start, 1.5));
        e.selectAll(false);
        assert(!e.nudgeSelected(later, 0));

        // split and copy keep velocity, channel and mute
        {
            NoteEditor f;
            Note n = {1, 2, 60, true, 0, "la"};
            n.vel = 25;
            n.chan = 3;
            n.muted = true;
            f.notes = {n};
            assert(f.splitAt(1.5) && f.notes[1].vel == 25 && f.notes[1].chan == 3 &&
                   f.notes[1].muted && f.notes[1].lyric.empty() && f.notes[1].idx == -1);
            const std::vector<Note> c = f.copySelected();
            assert(c[0].vel == 25 && c[0].chan == 3 && c[0].muted && c[0].idx == -1);
        }

        // split: the given note, or selected notes spanning t; not too close to an edge
        e.notes = {{1, 2, 60, false, 0, "la"}};
        assert(!e.splitAt(1.5)); // nothing selected
        assert(e.splitAt(1.5, 0) && e.notes.size() == 2 && near(e.notes[0].end, 1.5) &&
               near(e.notes[1].start, 1.5));
        assert(e.notes[0].lyric == "la" && e.notes[1].lyric.empty() && e.notes[1].pitch == 60);
        assert(!e.splitAt(1.52, 1)); // too close to the edge

        // join: one note from the first start to the last end, at the first note's pitch
        e.notes = {{2, 3, 62, true, 4, "lo"}, {1, 2, 60, true, 3, "hel-"}, {4, 5, 60, false, 5}};
        e.deleted.clear();
        assert(e.joinSelected() && e.notes.size() == 2);
        assert(near(e.notes[0].start, 1) && near(e.notes[0].end, 3) && e.notes[0].pitch == 60 &&
               e.notes[0].lyric == "hello");
        assert(e.deleted.size() == 1 && e.deleted[0] == 4);
    }

    printf("ok\n");
}
