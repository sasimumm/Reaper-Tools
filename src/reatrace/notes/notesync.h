// Notes <-> MIDI take, independent of REAPER: the editor's notes are read from and written back to
// a Take, which takesync.cpp implements over REAPER's MIDI API and tests/notesync_test.cpp fakes.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "notes/editor.h"

// A MIDI take's notes and text events, by index. Positions are in PPQ (MIDI ticks); toTime/toPPQ
// convert to and from project seconds.
class Take {
  public:
    struct NoteEvent {
        double start, end; // PPQ
        int pitch;
        bool sel;
        int vel = 96, chan = 0;
        bool muted = false;
    };
    struct TextEvent {
        double ppq = 0;
        int type = 0; // MIDI meta event type, e.g. LYRIC_EVENT
        std::string text;
        bool sel = false, muted = false;
    };

    virtual ~Take() = default;
    virtual const void *identity() = 0; // which take this is
    virtual std::string hash() = 0;     // changes with any edit of its events
    // Changes whenever toTime() would: the item moved, its start offset or rate changed, the tempo
    // map changed. Notes are kept in project seconds, so this matters as much as the hash.
    virtual std::string timeMapping() = 0;
    virtual double toTime(double ppq) = 0;
    virtual double toPPQ(double time) = 0;
    virtual int noteCount() = 0;
    virtual NoteEvent note(int i) = 0;
    virtual int textCount() = 0;
    virtual bool text(int i, TextEvent *out) = 0; // false if it isn't a text event
    // Changes don't re-sort until sort(), so indices stay valid while writing.
    virtual void selectNote(int i, bool sel) = 0; // selection only: the event is left as it is
    virtual void deleteNote(int i) = 0;
    virtual void insertNote(const NoteEvent &n) = 0;
    virtual void deleteText(int i) = 0;
    virtual void insertText(const TextEvent &t) = 0;
    virtual void sort() = 0;

  protected:
    // Copied only as a whole fake or real take, never through this interface.
    Take() = default;
    Take(const Take &) = default;
    Take &operator=(const Take &) = default;
};

static const int LYRIC_EVENT = 5; // MIDI meta event type used by REAPER's notation lyrics

// What the loaded notes were read from: writing them back is only safe while this still matches,
// since they refer to the take's events by index and keep times in project seconds.
struct TakeSnapshot {
    const void *take = nullptr;
    std::string hash, timeMapping;
    bool operator==(const TakeSnapshot &o) const {
        return take == o.take && hash == o.hash && timeMapping == o.timeMapping;
    }
    bool operator!=(const TakeSnapshot &o) const { return !(*this == o); }
};
TakeSnapshot snapshotOf(Take &take);

// The take's notes with Note::idx set to their take index; a lyric event attaches to the first
// note (by index) starting within half a tick of it.
std::vector<Note> readNotes(Take &take);

// MIDI can't hold two notes of one pitch and channel at once (a note-off would end either), so where
// a note drawn, moved, resized or transposed since `loaded` overlaps another, the earlier is cut to
// end where the later starts, and notes starting together become one (dropped notes read from the
// take are added to `deleted`). Overlaps already in the take are left. True if any changed.
bool trimOverlaps(std::vector<Note> &notes, std::vector<int> &deleted,
                  std::span<const Note> loaded);

// What writing `edited` over `loaded` (as read by readNotes) would change.
struct NoteChanges {
    bool data = false;   // notes or lyrics: worth an undo point (selection alone isn't)
    bool lyrics = false; // the lyric events need rewriting
};
NoteChanges diffNotes(std::span<const Note> loaded, std::span<const Note> edited,
                      std::span<const int> deleted);

// Writes the edits, unless the take changed since `loadedFrom` (then nothing is written and it
// returns false: the caller reloads). Moved, resized, transposed and new notes are put on whole
// ticks and their overlaps resolved there (trimOverlaps); then `deleted` take indices are removed
// (highest first), retimed notes removed and reinserted, re-selected ones selected, new notes
// (idx < 0) inserted (with `velocity` unless they have their own). If lyrics need rewriting, the lyric events attached to loaded notes are replaced by
// one per edited note with a lyric, in the sorted take's order (with empty ones on lyric-less notes
// before them in a group starting together, so each pairs with its own note); other text events
// are left alone.
bool writeNotes(Take &take, const TakeSnapshot &loadedFrom, std::span<const Note> loaded,
                std::span<const Note> edited, std::span<const int> deleted, int velocity);

// Adds unselected notes (another voice's accepted suggestions), trimmed against the take's notes
// as trimOverlaps does.
void appendNotes(Take &take, std::span<const Note> notes, int velocity);
