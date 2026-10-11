// Notes <-> MIDI take, independent of REAPER: the editor's notes are read from and written back to
// a Take, which takesync.cpp implements over REAPER's MIDI API and tests/notesync_test.cpp fakes.
#pragma once

#include <string>
#include <vector>

#include "editor.h"

// A MIDI take's notes and text events, by index. Positions are in PPQ (MIDI ticks); toTime/toPPQ
// convert to and from project seconds.
class Take {
  public:
    struct NoteEvent {
        double start, end; // PPQ
        int pitch;
        bool sel;
    };
    struct TextEvent {
        double ppq = 0;
        int type = 0; // MIDI meta event type, e.g. LYRIC_EVENT
        std::string text;
    };

    virtual ~Take() = default;
    virtual const void *identity() = 0; // which take this is
    virtual std::string hash() = 0;     // changes with any edit of its events
    // Changes whenever toTime() would: the item moved, its start offset or rate changed, the tempo map
    // changed. Notes are kept in project seconds, so this matters as much as the hash.
    virtual std::string timeMapping() = 0;
    virtual double toTime(double ppq) = 0;
    virtual double toPPQ(double time) = 0;
    virtual int noteCount() = 0;
    virtual NoteEvent note(int i) = 0;
    virtual int textCount() = 0;
    virtual bool text(int i, TextEvent *out) = 0; // false if it isn't a text event
    // Changes don't re-sort until sort(), so indices stay valid while writing.
    virtual void setNote(int i, const NoteEvent &n) = 0;
    virtual void deleteNote(int i) = 0;
    virtual void insertNote(const NoteEvent &n, int velocity) = 0;
    virtual void deleteText(int i) = 0;
    virtual void insertText(const TextEvent &t) = 0;
    virtual void sort() = 0;
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

// What writing `edited` over `loaded` (as read by readNotes) would change.
struct NoteChanges {
    bool data = false;   // notes or lyrics: worth an undo point (selection alone isn't)
    bool lyrics = false; // the lyric events need rewriting
};
NoteChanges diffNotes(const std::vector<Note> &loaded, const std::vector<Note> &edited,
                      const std::vector<int> &deleted);

// Writes the edits, unless the take changed since `loadedFrom` (then nothing is written and it
// returns false: the caller reloads). Moved, resized and re-selected notes are set, `deleted` take indices removed
// (highest first), new notes (idx < 0) inserted. If lyrics changed, the lyric events attached to
// loaded notes are replaced by one per edited note with a lyric; other text events are left alone.
bool writeNotes(Take &take, const TakeSnapshot &loadedFrom, const std::vector<Note> &loaded,
                const std::vector<Note> &edited, const std::vector<int> &deleted, int velocity);

// Adds unselected notes (another voice's accepted suggestions).
void appendNotes(Take &take, const std::vector<Note> &notes, int velocity);
