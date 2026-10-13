// Reading the editor's notes from a Take and writing edits back, independent of REAPER.
#include "notes/notesync.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <span>

TakeSnapshot snapshotOf(Take &take) { return {take.identity(), take.hash(), take.timeMapping()}; }

std::vector<Note> readNotes(Take &take) {
    std::vector<Note> notes;
    std::vector<double> startPPQ;
    const int count = take.noteCount();
    for (int i = 0; i < count; i++) {
        const Take::NoteEvent e = take.note(i);
        Note n = {take.toTime(e.start), take.toTime(e.end), e.pitch, e.sel, i};
        n.vel = e.vel;
        n.chan = e.chan;
        n.muted = e.muted;
        notes.push_back(n);
        startPPQ.push_back(e.start);
    }

    // notes by start (ties by index), to find each lyric's note without scanning them all
    std::vector<size_t> byStart(notes.size());
    std::iota(byStart.begin(), byStart.end(), size_t{0});
    std::stable_sort(byStart.begin(), byStart.end(),
                     [&](size_t a, size_t b) { return startPPQ[a] < startPPQ[b]; });

    const int texts = take.textCount();
    for (int i = 0; i < texts; i++) {
        Take::TextEvent t;
        if (!take.text(i, &t) || t.type != LYRIC_EVENT) {
            continue;
        }
        // the first note (by index) starting within half a tick that has no lyric yet
        auto it = std::lower_bound(byStart.begin(), byStart.end(), t.ppq - 0.5,
                                   [&](size_t n, double p) { return startPPQ[n] <= p; });
        for (; it != byStart.end() && startPPQ[*it] < t.ppq + 0.5; ++it) {
            if (notes[*it].lyricIdx < 0) {
                notes[*it].lyric = t.text;
                notes[*it].lyricIdx = i;
                break;
            }
        }
    }
    return notes;
}

// Whether n was read from the take (it is loaded[n.idx]), rather than added since.
static bool known(const Note &n, std::span<const Note> loaded) {
    return n.idx >= 0 && n.idx < static_cast<int>(loaded.size());
}

// n as a take note event, with its times in PPQ; `velocity` if n has none of its own.
static Take::NoteEvent toEvent(Take &take, const Note &n, bool sel, int velocity) {
    Take::NoteEvent e = {take.toPPQ(n.start), take.toPPQ(n.end), n.pitch, sel};
    e.vel = n.vel >= 0 ? n.vel : velocity;
    e.chan = n.chan;
    e.muted = n.muted;
    return e;
}

NoteChanges diffNotes(std::span<const Note> loaded, std::span<const Note> edited,
                      std::span<const int> deleted) {
    NoteChanges c;
    c.data = !deleted.empty();
    // Lyrics pair with notes starting together by order, which inserting, deleting, moving or
    // transposing any note can change: then they are rewritten, if there are any.
    bool reordered = !deleted.empty(), anyLyric = false;
    for (const Note &o : loaded) {
        anyLyric |= o.lyricIdx >= 0;
    }
    for (const Note &n : edited) {
        anyLyric |= !n.lyric.empty();
        if (!known(n, loaded)) {
            c.data = reordered = true;
            continue;
        }
        const Note &o = loaded[static_cast<size_t>(n.idx)];
        c.data |= n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.lyric != o.lyric;
        c.lyrics |= n.lyric != o.lyric;
        reordered |= n.start != o.start || n.pitch != o.pitch;
    }
    c.lyrics |= reordered && anyLyric;
    return c;
}

// Inserts a lyric event per edited note with a lyric, into the sorted take. readNotes gives a lyric
// to the first free note (in take order) starting within half a tick, so in a group of notes
// starting together, each note before the last one with a lyric gets one too: empty if it has none.
static void writeLyrics(Take &take, std::span<const Note> edited) {
    // edited notes' lyrics by (start tick, pitch), to find them from the take's notes
    std::map<std::pair<long long, int>, std::vector<const std::string *>> lyricOf;
    for (const Note &n : edited) {
        lyricOf[{std::llround(take.toPPQ(n.start)), n.pitch}].push_back(&n.lyric);
    }
    const int count = take.noteCount();
    for (int i = 0; i < count;) {
        // the group starting with note i
        const double groupStart = take.note(i).start;
        std::vector<std::pair<double, const std::string *>> group; // (start, lyric)
        for (; i < count; i++) {
            const Take::NoteEvent e = take.note(i);
            if (e.start >= groupStart + 0.5) {
                break;
            }
            std::vector<const std::string *> &l = lyricOf[{std::llround(e.start), e.pitch}];
            group.push_back({e.start, l.empty() ? nullptr : l.front()});
            if (!l.empty()) {
                l.erase(l.begin());
            }
        }
        size_t last = group.size();
        while (last > 0 && (!group[last - 1].second || group[last - 1].second->empty())) {
            last--;
        }
        for (size_t k = 0; k < last; k++) {
            const std::string *l = group[k].second;
            take.insertText({group[k].first, LYRIC_EVENT, l ? *l : std::string()});
        }
    }
}

bool writeNotes(Take &take, const TakeSnapshot &loadedFrom, std::span<const Note> loaded,
                std::span<const Note> edited, std::span<const int> deleted, int velocity) {
    if (snapshotOf(take) != loadedFrom) {
        return false;
    }
    const bool lyrics = diffNotes(loaded, edited, deleted).lyrics;

    // changed notes in place, then deletions, then new notes: indices stay valid until sort()
    for (const Note &n : edited) {
        if (!known(n, loaded)) {
            continue;
        }
        const Note &o = loaded[static_cast<size_t>(n.idx)];
        if (n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.sel != o.sel) {
            take.setNote(n.idx, toEvent(take, n, n.sel, velocity));
        }
    }
    std::vector<int> del(deleted.begin(), deleted.end());
    std::sort(del.rbegin(), del.rend()); // highest first so earlier indices stay valid
    for (int i : del) {
        take.deleteNote(i);
    }
    for (const Note &n : edited) {
        if (n.idx < 0) {
            take.insertNote(toEvent(take, n, n.sel, velocity));
        }
    }

    if (lyrics) {
        std::vector<int> old;
        for (const Note &o : loaded) {
            if (o.lyricIdx >= 0) {
                old.push_back(o.lyricIdx);
            }
        }
        std::sort(old.rbegin(), old.rend());
        for (int i : old) {
            take.deleteText(i);
        }
    }
    take.sort();
    if (lyrics) {
        writeLyrics(take, edited);
        take.sort();
    }
    return true;
}

void appendNotes(Take &take, std::span<const Note> notes, int velocity) {
    for (const Note &n : notes) {
        take.insertNote(toEvent(take, n, false, velocity));
    }
    take.sort();
}
