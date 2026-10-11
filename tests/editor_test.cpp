// Self-check for note editing gestures: run `build/editor_test`.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/editor.h"

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

int main()
{
  ViewMap v; // 100 px/s, 12 px/semitone, pitch 84.5 at y=0
  NoteEditor ed;
  ed.snap = [](double t) { return round(t * 4) / 4; }; // quarter-second grid

  // Drawing on empty space creates a snapped note at the row's pitch.
  const double y60 = v.y(60);
  ed.mouseDown(v, v.x(1.02), y60, 0);
  ed.mouseMove(v, v.x(1.4), y60, 0);
  assert(ed.drawing() == 0);
  assert(ed.mouseUp(v, v.x(1.9), y60, 0));
  assert(ed.notes.size() == 1 && ed.notes[0].pitch == 60 && ed.notes[0].sel && ed.notes[0].idx == -1);
  assert(near(ed.notes[0].start, 1.0) && near(ed.notes[0].end, 2.0));

  // A click on empty space without dragging deselects and draws nothing.
  ed.mouseDown(v, v.x(5), y60, 0);
  assert(ed.mouseUp(v, v.x(5) + 1, y60, 0));
  assert(ed.notes.size() == 1 && !ed.notes[0].sel);

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

  // Shift locks a move to the axis it mostly went along, once past 10 px; releasing shift unlocks.
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

  // Moving several selected notes keeps their spacing; Delete records take indices.
  ed.selectAll(true);
  ed.mouseDown(v, v.x(3.5), v.y(60), 0);
  ed.mouseUp(v, v.x(4.0), v.y(60), 0);
  assert(near(ed.notes[0].start, 1.5) && near(ed.notes[2].start, 5.5));
  ed.notes[2].sel = ed.notes[0].sel = false;
  assert(ed.deleteSelected() && ed.notes.size() == 2 && ed.deleted.size() == 1 && ed.deleted[0] == 1);

  // Cancel restores the notes from before the gesture.
  ed.mouseDown(v, v.x(1.75), v.y(60), 0);
  ed.mouseMove(v, v.x(3), v.y(70), 0);
  ed.cancel();
  assert(near(ed.notes[0].start, 1.5) && ed.notes[0].pitch == 60);

  // Narrow notes have no resize edges.
  ed.notes = {{1, 1.05, 60, false, 0}};
  int i;
  assert(ed.hitTest(v, v.x(1.0), v.y(60), &i) == NoteEditor::BODY);
  // Pasted lyrics: one syllable per note, hyphens stay with their syllable.
  std::vector<std::string> syl = splitSyllables("Hel-lo  dar-k-ness\nmy _ old\tfriend- -");
  const char *want[] = {"Hel-", "lo", "dar-", "k-", "ness", "my", "_", "old", "friend-"};
  assert(syl.size() == 9);
  for (int k = 0; k < 9; k++) assert(syl[k] == want[k]);

  // Copy/paste at a time, duplicate after the selection, split, join (lyrics joined without hyphens).
  {
    NoteEditor e;
    e.notes = {{1, 2, 60, true, 0, "hel-", 3}, {2, 3, 62, true, 1, "lo"}, {5, 6, 64, false, 2}};
    const std::vector<Note> clip = e.copySelected();
    e.paste(clip, 10);
    assert(e.notes.size() == 5 && near(e.notes[3].start, 10) && near(e.notes[4].end, 12) && e.notes[3].idx == -1);
    assert(e.notes[3].lyric == "hel-" && e.notes[3].lyricIdx == -1 && e.notes[3].sel && !e.notes[0].sel);
    assert(e.duplicateSelected() && e.notes.size() == 7 && near(e.notes[5].start, 12) && near(e.notes[6].end, 14));

    e.notes = {{1, 2, 60, false, 0, "la"}};
    assert(!e.splitAt(1.5)); // nothing selected
    assert(e.splitAt(1.5, 0) && e.notes.size() == 2 && near(e.notes[0].end, 1.5) && near(e.notes[1].start, 1.5));
    assert(e.notes[0].lyric == "la" && e.notes[1].lyric.empty() && e.notes[1].pitch == 60);
    assert(!e.splitAt(1.52, 1)); // too close to the edge

    e.notes = {{2, 3, 62, true, 4, "lo"}, {1, 2, 60, true, 3, "hel-"}, {4, 5, 60, false, 5}};
    e.deleted.clear();
    assert(e.joinSelected() && e.notes.size() == 2);
    assert(near(e.notes[0].start, 1) && near(e.notes[0].end, 3) && e.notes[0].pitch == 60 && e.notes[0].lyric == "hello");
    assert(e.deleted.size() == 1 && e.deleted[0] == 4);
  }

  printf("ok\n");
}
