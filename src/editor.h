// Note editing gestures, independent of REAPER and the window (tested in tests/editor_test.cpp).
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

struct Note
{
  double start, end; // project time, seconds
  int pitch;
  bool sel;
  int idx; // index in the MIDI take, -1 for notes not written yet
  std::string lyric;
  int lyricIdx = -1; // index of the lyric text event in the take, -1 if none
};

// Maps window pixels to time (seconds) and continuous MIDI pitch. Note row P spans P-0.5 .. P+0.5.
struct ViewMap
{
  double t0 = 0, pps = 100;          // time at the note area's left edge, pixels per second
  double pitchTop = 84.5, rowH = 12; // pitch at the note area's top edge, pixels per semitone
  int x0 = 0, y0 = 0;                // note area origin in window pixels

  double x(double t) const { return x0 + (t - t0) * pps; }
  double t(double x) const { return t0 + (x - x0) / pps; }
  double y(double p) const { return y0 + (pitchTop - p) * rowH; }
  double p(double y) const { return pitchTop - (y - y0) / rowH; }
  int pitchAt(double y) const { return (int)floor(p(y) + 0.5); }
};

enum { MOD_SHIFT = 1, MOD_CMD = 2, MOD_ALT = 4 };

// No modes: drag on empty space draws, drag a note body moves, drag an edge resizes,
// shift+drag on empty space selects with a marquee, cmd+click toggles selection.
// alt disables snapping. Moves and resizes apply original + delta, snapped via the anchor note.
class NoteEditor
{
public:
  enum Hit { NONE, BODY, LEFT, RIGHT };

  std::vector<Note> notes;
  std::vector<int> deleted;                          // take indices deleted since notes were loaded
  std::function<double(double)> snap = [](double t) { return t; };
  double minLen = 0.05;                              // seconds
  int velocity = 96;

  Hit hitTest(const ViewMap &v, double x, double y, int *noteIndex) const;
  void mouseDown(const ViewMap &v, double x, double y, int mods);
  void mouseMove(const ViewMap &v, double x, double y, int mods);
  bool mouseUp(const ViewMap &v, double x, double y, int mods); // true if notes or selection changed
  void cancel();                                                 // abort the current gesture
  bool active() const { return mode != IDLE; }

  bool deleteSelected();
  bool selectAll(bool sel);
  bool deleteAt(const ViewMap &v, double x, double y);

  bool marquee(double r[4]) const; // marquee rectangle while selecting
  int drawing() const { return mode == DRAW ? drawIndex : -1; }
  // the note being drawn, moved or resized, -1 if none
  int dragNote() const { return mode == DRAW ? drawIndex : mode == MOVE || mode == RESIZE_L || mode == RESIZE_R ? anchor : -1; }

private:
  enum Mode { IDLE, PENDING_DRAW, DRAW, PENDING_DRAG, MOVE, RESIZE_L, RESIZE_R, MARQUEE };
  Mode mode = IDLE;
  Hit dragHit = NONE;
  double downX = 0, downY = 0, curX = 0, curY = 0;
  int anchor = -1, drawIndex = -1;
  bool changed = false, cmdMarquee = false;
  std::vector<Note> orig; // notes at mouse down
};
