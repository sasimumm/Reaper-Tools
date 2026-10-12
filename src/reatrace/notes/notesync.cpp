// Reading the editor's notes from a Take and writing edits back, independent of REAPER.
#include "notes/notesync.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <span>

TakeSnapshot snapshotOf(Take &take) { return {take.identity(), take.hash(), take.timeMapping()}; }

std::vector<Note> readNotes(Take &take) {
    std::vector<Note> notes;
    std::vector<double> startPPQ;
    const int count = take.noteCount();
    for (int i = 0; i < count; i++) {
        const Take::NoteEvent e = take.note(i);
        notes.push_back({take.toTime(e.start), take.toTime(e.end), e.pitch, e.sel, i});
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

// n as a take note event, with its times in PPQ.
static Take::NoteEvent toEvent(Take &take, const Note &n, bool sel) {
    return {take.toPPQ(n.start), take.toPPQ(n.end), n.pitch, sel};
}

NoteChanges diffNotes(std::span<const Note> loaded, std::span<const Note> edited,
                      std::span<const int> deleted) {
    NoteChanges c;
    c.data = !deleted.empty();
    for (int i : deleted) {
        c.lyrics |= i >= 0 && i < static_cast<int>(loaded.size()) &&
                    loaded[static_cast<size_t>(i)].lyricIdx >= 0;
    }
    for (const Note &n : edited) {
        if (!known(n, loaded)) {
            c.data = true;
            c.lyrics |= !n.lyric.empty();
            continue;
        }
        const Note &o = loaded[static_cast<size_t>(n.idx)];
        c.data |= n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.lyric != o.lyric;
        // a lyric event sits at its note's start, so it moves with it
        c.lyrics |= n.lyric != o.lyric || (n.start != o.start && !n.lyric.empty());
    }
    return c;
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
            take.setNote(n.idx, toEvent(take, n, n.sel));
        }
    }
    std::vector<int> del(deleted.begin(), deleted.end());
    std::sort(del.rbegin(), del.rend()); // highest first so earlier indices stay valid
    for (int i : del) {
        take.deleteNote(i);
    }
    for (const Note &n : edited) {
        if (n.idx < 0) {
            take.insertNote(toEvent(take, n, n.sel), velocity);
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
        for (const Note &n : edited) {
            if (!n.lyric.empty()) {
                take.insertText({take.toPPQ(n.start), LYRIC_EVENT, n.lyric});
            }
        }
    }
    take.sort();
    return true;
}

void appendNotes(Take &take, std::span<const Note> notes, int velocity) {
    for (const Note &n : notes) {
        take.insertNote(toEvent(take, n, false), velocity);
    }
    take.sort();
}
