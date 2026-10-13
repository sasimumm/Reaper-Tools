// Self-check for notes <-> MIDI take: run `build/notesync_test`.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "notes/notesync.h"

// A take like REAPER's: indices stay put until sort(), inserted events are appended, sort() orders
// notes by start then pitch and text events by position. 960 PPQ per second.
class FakeTake : public Take {
  public:
    std::vector<NoteEvent> notes; // in PPQ
    std::vector<TextEvent> texts;
    int sorts = 0;       // how many times sort() was called
    double position = 0; // where the item starts: moving it changes every note's project time

    const void *identity() override { return this; }

    // Every event, in order, as text: changes whenever any event does.
    std::string hash() override {
        std::string h;
        char buf[96];
        for (const NoteEvent &n : notes) {
            snprintf(buf, sizeof(buf), "n%g,%g,%d,%d;", n.start, n.end, n.pitch, n.sel ? 1 : 0);
            h += buf;
        }
        for (const TextEvent &t : texts) {
            snprintf(buf, sizeof(buf), "t%g,%d,", t.ppq, t.type);
            h += buf + t.text + ";";
        }
        return h;
    }

    std::string timeMapping() override { return std::to_string(position); }
    double toTime(double ppq) override { return position + ppq / 960; }
    double toPPQ(double time) override { return (time - position) * 960; }
    int noteCount() override { return static_cast<int>(notes.size()); }
    NoteEvent note(int i) override { return notes[static_cast<size_t>(i)]; }
    int textCount() override { return static_cast<int>(texts.size()); }

    bool text(int i, TextEvent *out) override {
        *out = texts[static_cast<size_t>(i)];
        return true;
    }

    void setNote(int i, const NoteEvent &n) override { notes[static_cast<size_t>(i)] = n; }
    void deleteNote(int i) override { notes.erase(notes.begin() + i); }
    void insertNote(const NoteEvent &n) override { notes.push_back(n); }
    void deleteText(int i) override { texts.erase(texts.begin() + i); }
    void insertText(const TextEvent &t) override { texts.push_back(t); }

    void sort() override {
        sorts++;
        std::stable_sort(notes.begin(), notes.end(), [](const NoteEvent &a, const NoteEvent &b) {
            return a.start < b.start || (a.start == b.start && a.pitch < b.pitch);
        });
        std::stable_sort(texts.begin(), texts.end(),
                         [](const TextEvent &a, const TextEvent &b) { return a.ppq < b.ppq; });
    }
};

static const int TRACK_NAME = 3; // another text event type, which must be left alone

// Loads, edits with `edit`, writes, reads back.
template <class F> static std::vector<Note> roundTrip(FakeTake &t, NoteChanges *changes, F edit) {
    const TakeSnapshot snap = snapshotOf(t);
    const std::vector<Note> loaded = readNotes(t);
    std::vector<Note> edited = loaded;
    std::vector<int> deleted;
    edit(edited, deleted);
    *changes = diffNotes(loaded, edited, deleted);
    assert(writeNotes(t, snap, loaded, edited, deleted, 96));
    return readNotes(t);
}

// Three notes, a second apart, two with lyrics; an orphan lyric and a track name.
static FakeTake song() {
    FakeTake t;
    t.notes = {{0, 480, 60, false, 96}, {960, 1440, 62, false, 96}, {1920, 2400, 64, false, 96}};
    t.texts = {
        {0, TRACK_NAME, "Lead"},
        {0, LYRIC_EVENT, "hel-"},
        {960, LYRIC_EVENT, "lo"},
        {3000, LYRIC_EVENT, "orphan"}
    };
    return t;
}

// The take's lyric events' texts, in order.
static std::vector<std::string> lyricTexts(const FakeTake &t) {
    std::vector<std::string> out;
    for (const Take::TextEvent &e : t.texts) {
        if (e.type == LYRIC_EVENT) {
            out.push_back(e.text);
        }
    }
    return out;
}

int main() {
    NoteChanges c;

    // reading: times converted, lyrics on the notes starting there; other text events ignored
    {
        FakeTake t = song();
        const std::vector<Note> n = readNotes(t);
        assert(n.size() == 3 && n[0].start == 0 && n[1].start == 1 && n[1].end == 1.5);
        assert(n[0].lyric == "hel-" && n[0].lyricIdx == 1 && n[1].lyric == "lo" &&
               n[2].lyric.empty());
        assert(n[0].idx == 0 && n[2].idx == 2);
    }

    // a chord: the lyric goes on the first note starting there, not both
    {
        FakeTake t;
        t.notes = {{0, 480, 60, false, 96}, {0, 480, 67, false, 96}};
        t.texts = {{0.2, LYRIC_EVENT, "ah"}}; // within half a tick
        const std::vector<Note> n = readNotes(t);
        assert(n[0].lyric == "ah" && n[1].lyric.empty());
    }

    // nothing changed: nothing written but the sort, no undo point
    {
        FakeTake t = song();
        const FakeTake before = t;
        roundTrip(t, &c, [](std::vector<Note> &, std::vector<int> &) {});
        assert(!c.data && !c.lyrics && t.notes.size() == before.notes.size() &&
               t.texts.size() == before.texts.size());
    }

    // selection only: written to the take, but no undo point and lyrics untouched
    {
        FakeTake t = song();
        const std::vector<Note> n =
            roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &) { e[2].sel = true; });
        assert(!c.data && !c.lyrics && n[2].sel && t.texts.size() == 4);
    }

    // moving a note with a lyric moves its lyric; the orphan lyric and the track name stay
    {
        FakeTake t = song();
        const std::vector<Note> n = roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &) {
            e[1].start = 2.5;
            e[1].end = 3;
        });
        assert(c.data && c.lyrics);
        assert(n.size() == 3 && n[2].pitch == 62 && n[2].start == 2.5 && n[2].lyric == "lo");
        assert(n[1].pitch == 64 && n[1].lyric.empty());
        const std::vector<std::string> l = lyricTexts(t);
        assert(std::count(l.begin(), l.end(), "orphan") == 1 &&
               std::count(l.begin(), l.end(), "lo") == 1);
        assert(t.texts[0].type == TRACK_NAME);
    }

    // deleting several notes removes the right ones (highest index first) and their lyrics
    {
        FakeTake t = song();
        const std::vector<Note> n = roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &d) {
            d = {0, 2};
            e = {e[1]};
        });
        assert(c.data && c.lyrics && n.size() == 1 && n[0].pitch == 62 && n[0].lyric == "lo");
        const std::vector<std::string> l = lyricTexts(t);
        assert(l.size() == 2 && std::count(l.begin(), l.end(), "hel-") == 0);
    }

    // new notes are inserted with the velocity and their lyric; renaming a lyric rewrites it
    {
        FakeTake t = song();
        const std::vector<Note> n = roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &) {
            e.push_back({4, 4.5, 67, true, -1, "world"});
            e[0].lyric = "hey";
        });
        assert(c.data && c.lyrics && n.size() == 4 && n[3].pitch == 67 && n[3].sel &&
               n[3].lyric == "world");
        assert(n[0].lyric == "hey" && t.notes[3].vel == 96);
        assert(lyricTexts(t).size() == 4); // hey, lo, orphan, world
    }

    // a note's velocity, channel and mute are read, and kept by notes made from it (a split half)
    {
        FakeTake t = song();
        t.notes[0].vel = 25;
        t.notes[0].chan = 3;
        t.notes[0].muted = true;
        const std::vector<Note> n = roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &) {
            Note half = e[0];
            half.start = 0.25;
            half.idx = half.lyricIdx = -1;
            half.lyric.clear();
            e[0].end = 0.25;
            e.push_back(half);
        });
        assert(n.size() == 4 && n[1].start == 0.25 && n[1].vel == 25 && n[1].chan == 3 &&
               n[1].muted && n[0].vel == 25);
    }

    // a resize alone doesn't touch the lyrics
    {
        FakeTake t = song();
        roundTrip(t, &c, [](std::vector<Note> &e, std::vector<int> &) { e[0].end = 0.75; });
        assert(c.data && !c.lyrics && t.notes[0].end == 720);
    }

    // the take changing after the notes were loaded makes writing them back refuse, leaving the
    // take as it is: the loaded notes refer to its events by index and keep times in project
    // seconds
    {
        // writing refused, and the take untouched
        auto refused = [](FakeTake &t, const TakeSnapshot &snap, std::span<const Note> loaded,
                          std::span<const Note> edited, const std::vector<int> &deleted = {}) {
            const std::string before = t.hash();
            const bool wrote = writeNotes(t, snap, loaded, edited, deleted, 96);
            return !wrote && t.hash() == before;
        };

        // the item moved (or the tempo changed): selecting a note must not write its old times
        // back
        FakeTake t = song();
        TakeSnapshot snap = snapshotOf(t);
        std::vector<Note> loaded = readNotes(t), edited = loaded;
        t.position = 2;
        assert(snapshotOf(t) != snap);
        edited[0].sel = true;
        assert(refused(t, snap, loaded, edited));
        assert(readNotes(t)[0].start == 2); // reloading gives the moved times

        // an action inserted a note during a drag: index 1 is now a different note
        t = song();
        snap = snapshotOf(t);
        loaded = readNotes(t);
        edited = loaded;
        t.insertNote({480, 700, 70, false});
        t.sort();
        edited[1].pitch = 63;
        assert(refused(t, snap, loaded, edited));

        // another take altogether (same events)
        FakeTake other = song();
        assert(refused(other, snapshotOf(t), readNotes(other), readNotes(other)));

        // unchanged: written
        t = song();
        snap = snapshotOf(t);
        loaded = readNotes(t);
        edited = loaded;
        edited[0].pitch = 61;
        assert(writeNotes(t, snap, loaded, edited, {}, 96) && readNotes(t)[0].pitch == 61);
    }

    // another voice's accepted notes are appended unselected, then sorted
    {
        FakeTake t = song();
        appendNotes(t, std::vector<Note>{{0.5, 0.75, 70, true, -1}}, 80);
        assert(t.notes.size() == 4 && t.notes[1].pitch == 70 && !t.notes[1].sel &&
               t.notes[1].vel == 80);
    }

    printf("ok\n");
}
