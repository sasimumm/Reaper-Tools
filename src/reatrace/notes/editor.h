// Note editing gestures, independent of REAPER and the window (tested in tests/editor_test.cpp).
#pragma once

#include <cmath>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A note in the editor.
struct Note {
    double start, end; // project time, seconds
    int pitch;         // MIDI note number
    bool sel;
    int idx; // index in the MIDI take, -1 for notes not written yet
    std::string lyric = {};
    int lyricIdx = -1; // index of the lyric text event in the take, -1 if none
    bool lyricSel = false, lyricMuted = false; // the lyric event's own flags, kept when rewritten
    int vel = -1;      // MIDI velocity, -1 for the editor's default
    int chan = 0;      // MIDI channel, 0..15
    bool muted = false;
};

// Maps window pixels to time (seconds) and continuous MIDI pitch. Note row P spans P-0.5 .. P+0.5.
struct ViewMap {
    double t0 = 0, pps = 100;          // time at the note area's left edge, pixels per second
    double pitchTop = 84.5, rowH = 12; // pitch at the note area's top edge, pixels per semitone
    int x0 = 0, y0 = 0;                // note area origin in window pixels

    double x(double t) const { return x0 + (t - t0) * pps; }
    double t(double x) const { return t0 + (x - x0) / pps; }
    double y(double p) const { return y0 + (pitchTop - p) * rowH; }
    double p(double y) const { return pitchTop - (y - y0) / rowH; }
    int pitchAt(double y) const { return static_cast<int>(floor(p(y) + 0.5)); }
};

enum { MOD_SHIFT = 1, MOD_CMD = 2, MOD_ALT = 4 };

// Splits pasted lyrics into one syllable per note: words split on whitespace, and after each
// hyphen, which stays with its syllable ("hel-lo world" -> "hel-", "lo", "world").
std::vector<std::string> splitSyllables(const std::string &text);

// No modes: drag on empty space draws, drag a note body moves, drag an edge resizes,
// shift+drag on empty space selects with a marquee, cmd+click toggles selection, cmd+drag on a
// note moves copies of the selection (and that note), leaving the originals.
// alt disables snapping, shift locks a move to time or pitch. Moves and resizes apply original +
// delta, snapped via the anchor note. Mouse positions are window pixels, mods MOD_* flags.
class NoteEditor {
  public:
    enum Hit { NONE, BODY, LEFT, RIGHT };

    std::vector<Note> notes;
    std::vector<int> deleted; // take indices deleted since notes were loaded
    std::function<double(double)> snap = [](double t) { return t; }; // seconds to seconds
    // Clone drags move in quarter notes, not seconds, so copies keep their place in the bar across
    // tempo changes, and by whole steps of cloneStep (quarter notes to quarter notes) instead of
    // snapping the pressed note: no onsets. Alt moves them freely.
    std::function<double(double)> toQN = [](double t) { return t; }, fromQN = toQN;
    std::function<double(double)> cloneStep = [](double qn) { return qn; };
    double minLen = 0.05; // seconds
    int velocity = 96;
    // A click on empty space places a note as long as the last one drawn or pressed (when it has
    // one); without this, or without such a note, the click only deselects. Alt allows it too.
    bool clickPlaces = false;
    double lastLen = 0; // seconds, 0 until a note is drawn or pressed
    int moves = 0;      // counts mouse moves, so caches of the notes can tell a drag step happened

    // What is at (x, y), the topmost note's index in *noteIndex.
    Hit hitTest(const ViewMap &v, double x, double y, int *noteIndex) const;
    void mouseDown(const ViewMap &v, double x, double y, int mods);
    void mouseMove(const ViewMap &v, double x, double y, int mods);
    // true if notes or selection changed
    bool mouseUp(const ViewMap &v, double x, double y, int mods);
    void cancel(); // abort the current gesture
    bool active() const { return mode != IDLE; }
    // pressed on a note's edge, or dragging it
    bool resizing() const {
        return mode == RESIZE_L || mode == RESIZE_R || (mode == PENDING_DRAG && dragHit != BODY);
    }

    // These return true if anything changed; deleted notes' take indices go to `deleted`.
    bool deleteSelected();
    bool clearLyrics(); // of the selected notes, keeping the notes
    bool selectAll(bool sel);
    bool deleteAt(const ViewMap &v, double x, double y); // the note at (x, y)

    // Editing commands; all return true if notes changed. New notes come out selected.
    std::vector<Note> copySelected() const; // as unwritten notes, lyrics and MIDI properties kept
    // Earliest clip note starts at `at`, the others the same number of quarter notes (toQN) after
    // it as before. clip must not view `notes`, which this appends to.
    void paste(std::span<const Note> clip, double at);
    bool duplicateSelected();              // right after the selection
    bool splitAt(double t, int note = -1); // that note, or selected notes spanning t
    bool joinSelected();                   // one note from the first's start to the last's end
    // Arrow keys: selected notes' starts and ends through `move`, pitches by `semitones`; nothing
    // moves if any note would leave 0..127, start before 0 or not end after its start.
    bool nudgeSelected(const std::function<double(double)> &move, int semitones);

    struct Box {
        double x0, y0, x1, y1; // window pixels, x0 <= x1 and y0 <= y1
    };
    std::optional<Box> marquee() const; // the marquee while selecting
    int drawing() const { return mode == DRAW ? drawIndex : -1; }
    // the note being drawn, moved or resized, -1 if none
    int dragNote() const {
        if (mode == DRAW) {
            return drawIndex;
        }
        if (mode == MOVE || mode == RESIZE_L || mode == RESIZE_R) {
            return anchor;
        }
        return -1;
    }

  private:
    enum Mode { IDLE, PENDING_DRAW, DRAW, PENDING_DRAG, MOVE, RESIZE_L, RESIZE_R, MARQUEE };
    Mode mode = IDLE;
    Hit dragHit = NONE;                              // where the pending drag was pressed
    double downX = 0, downY = 0, curX = 0, curY = 0; // window pixels
    int anchor = -1, drawIndex = -1; // the note pressed (it snaps for all), the note being drawn
    bool changed = false;            // since mouse down
    bool cmdMarquee = false;         // the marquee adds to the selection
    enum Axis { AXIS_FREE, AXIS_TIME, AXIS_PITCH } axis = AXIS_FREE; // shift+move: only along this
    std::vector<Note> orig;                                          // notes at mouse down
    bool cmdPress = false;         // pressed a note with cmd: a click toggles, a drag clones
    std::vector<Note> beforeClone; // notes at mouse down while clone dragging (orig has the copies)
};
