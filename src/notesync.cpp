#include "notesync.h"

#include <algorithm>
#include <cmath>

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
    const int texts = take.textCount();
    for (int i = 0; i < texts; i++) {
        Take::TextEvent t;
        if (!take.text(i, &t) || t.type != LYRIC_EVENT) {
            continue;
        }
        for (size_t n = 0; n < notes.size(); n++) {
            if (notes[n].lyricIdx < 0 && fabs(startPPQ[n] - t.ppq) < 0.5) {
                notes[n].lyric = t.text, notes[n].lyricIdx = i;
                break;
            }
        }
    }
    return notes;
}

static bool known(const Note &n, const std::vector<Note> &loaded) {
    return n.idx >= 0 && n.idx < (int)loaded.size();
}

NoteChanges diffNotes(const std::vector<Note> &loaded, const std::vector<Note> &edited,
                      const std::vector<int> &deleted) {
    NoteChanges c;
    c.data = !deleted.empty();
    for (int i : deleted) {
        c.lyrics |= i >= 0 && i < (int)loaded.size() && loaded[(size_t)i].lyricIdx >= 0;
    }
    for (const Note &n : edited) {
        if (!known(n, loaded)) {
            c.data = true, c.lyrics |= !n.lyric.empty();
            continue;
        }
        const Note &o = loaded[(size_t)n.idx];
        c.data |= n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.lyric != o.lyric;
        c.lyrics |= n.lyric != o.lyric || (n.start != o.start && !n.lyric.empty());
    }
    return c;
}

bool writeNotes(Take &take, const TakeSnapshot &loadedFrom, const std::vector<Note> &loaded,
                const std::vector<Note> &edited, const std::vector<int> &deleted, int velocity) {
    if (snapshotOf(take) != loadedFrom) {
        return false;
    }
    const bool lyrics = diffNotes(loaded, edited, deleted).lyrics;
    for (const Note &n : edited) {
        if (!known(n, loaded)) {
            continue;
        }
        const Note &o = loaded[(size_t)n.idx];
        if (n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.sel != o.sel) {
            take.setNote(n.idx, {take.toPPQ(n.start), take.toPPQ(n.end), n.pitch, n.sel});
        }
    }
    std::vector<int> del = deleted;
    std::sort(del.rbegin(), del.rend()); // highest first so earlier indices stay valid
    for (int i : del) {
        take.deleteNote(i);
    }
    for (const Note &n : edited) {
        if (n.idx < 0) {
            take.insertNote({take.toPPQ(n.start), take.toPPQ(n.end), n.pitch, n.sel}, velocity);
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

void appendNotes(Take &take, const std::vector<Note> &notes, int velocity) {
    for (const Note &n : notes) {
        take.insertNote({take.toPPQ(n.start), take.toPPQ(n.end), n.pitch, false}, velocity);
    }
    take.sort();
}
