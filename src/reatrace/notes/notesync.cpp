// Reading the editor's notes from a Take and writing edits back, independent of REAPER.
#include "notes/notesync.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <span>
#include <tuple>
#include <utility>

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
    e.end = std::max(e.end, e.start + 1); // at least a tick: a note-off before its note-on would
                                          // pair with the next note's
    e.vel = n.vel >= 0 ? n.vel : velocity;
    e.chan = n.chan;
    e.muted = n.muted;
    return e;
}

// Whether n was drawn, moved, resized or transposed since it was read from the take.
static bool retimed(const Note &n, std::span<const Note> loaded) {
    if (!known(n, loaded)) {
        return true;
    }
    const Note &o = loaded[static_cast<size_t>(n.idx)];
    return n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.chan != o.chan;
}

bool trimOverlaps(std::vector<Note> &notes, std::vector<int> &deleted,
                  std::span<const Note> loaded) {
    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Note &x = notes[a], &y = notes[b];
        return std::tie(x.chan, x.pitch, x.start) < std::tie(y.chan, y.pitch, y.start);
    });
    std::vector<bool> moved(notes.size()), drop(notes.size());
    for (size_t i = 0; i < notes.size(); i++) {
        moved[i] = retimed(notes[i], loaded);
    }
    bool any = false;
    std::vector<size_t> open; // kept notes of b's pitch and channel that may still sound at b
    for (const size_t k : order) {
        const Note &b = notes[k];
        std::erase_if(open, [&](size_t a) {
            return notes[a].chan != b.chan || notes[a].pitch != b.pitch || notes[a].end <= b.start;
        });
        for (const size_t ai : open) {
            Note &a = notes[ai];
            if (!moved[ai] && !moved[k]) {
                continue; // the take's own overlap: not ours to change
            }
            if (a.start == b.start) {
                // a duplicate: one note, as long as the longer, keeping a lyric and selection
                a.end = std::max(a.end, b.end);
                if (a.lyric.empty()) {
                    a.lyric = b.lyric;
                }
                a.sel |= b.sel;
                drop[k] = any = true;
                break;
            }
            a.end = b.start;
            any = true;
        }
        if (!drop[k]) {
            open.push_back(k);
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < notes.size(); i++) {
        if (!drop[i]) {
            if (w != i) { // moving a string onto itself empties it
                notes[w] = std::move(notes[i]);
            }
            w++;
        } else if (notes[i].idx >= 0) {
            deleted.push_back(notes[i].idx);
        }
    }
    notes.resize(w);
    return any;
}

NoteChanges diffNotes(std::span<const Note> loaded, std::span<const Note> edited,
                      std::span<const int> deleted) {
    NoteChanges c;
    c.data = !deleted.empty();
    // Lyrics pair with notes starting together by order, which inserting, deleting or retiming
    // (reinserting) any note can change: then they are rewritten, if there are any.
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
        reordered |= retimed(n, loaded); // reinserted: its place among notes starting together may change
    }
    c.lyrics |= reordered && anyLyric;
    return c;
}

// Inserts a lyric event per edited note with a lyric, into the sorted take. readNotes gives a lyric
// to the first free note (in take order) starting within half a tick, so in a group of notes
// starting together, each note before the last one with a lyric gets one too: empty if it has none.
static void writeLyrics(Take &take, std::span<const Note> edited) {
    // edited notes' lyrics by channel and pitch, with their starts in PPQ, to find them from the take's notes:
    // the take keeps whole ticks, rounded its own way, so a start matches within a tick
    struct Lyric {
        double start;
        const std::string *text;
        bool used = false;
    };
    std::map<std::pair<int, int>, std::vector<Lyric>> lyricOf; // by (channel, pitch)
    for (const Note &n : edited) {
        if (!n.lyric.empty()) {
            lyricOf[{n.chan, n.pitch}].push_back({take.toPPQ(n.start), &n.lyric});
        }
    }
    // the closest unused lyric of this channel and pitch within a tick of start
    const auto find = [&](double start, int chan, int pitch) -> const std::string * {
        Lyric *best = nullptr;
        for (Lyric &l : lyricOf[{chan, pitch}]) {
            const double d = fabs(l.start - start);
            if (!l.used && d < 1 && (!best || d < fabs(best->start - start))) {
                best = &l;
            }
        }
        if (!best) {
            return nullptr;
        }
        best->used = true;
        return best->text;
    };

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
            group.push_back({e.start, find(e.start, e.chan, e.pitch)});
        }
        size_t last = group.size();
        while (last > 0 && !group[last - 1].second) {
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

    // Deletions first, while the take is sorted as loaded: REAPER pairs note-ons with note-offs
    // by order, so deleting from a take with unsorted changes can take another note's note-off,
    // leaving a note that sounds to the item's end. Highest index first, so the rest stay valid.
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
    // Retimed notes are deleted and reinserted too: setting several notes' positions in place
    // can leave REAPER pairing note-ons with the wrong note-offs.
    std::vector<int> del(deleted.begin(), deleted.end());
    for (const Note &n : edited) {
        if (known(n, loaded) && retimed(n, loaded)) {
            del.push_back(n.idx);
        }
    }
    std::sort(del.rbegin(), del.rend());
    del.erase(std::unique(del.begin(), del.end()), del.end());
    for (int i : del) {
        take.deleteNote(i);
    }
    std::sort(del.begin(), del.end());

    // then selection changes in place (indices less the deleted notes before them), and new and
    // retimed notes, unsorted until sort()
    for (const Note &n : edited) {
        if (!known(n, loaded) || retimed(n, loaded)) {
            continue;
        }
        const Note &o = loaded[static_cast<size_t>(n.idx)];
        if (n.sel != o.sel) {
            const auto before = std::lower_bound(del.begin(), del.end(), n.idx) - del.begin();
            take.setNote(n.idx - static_cast<int>(before), toEvent(take, n, n.sel, velocity));
        }
    }
    for (const Note &n : edited) {
        if (!known(n, loaded) || retimed(n, loaded)) {
            take.insertNote(toEvent(take, n, n.sel, velocity));
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
    // through writeNotes, so they're trimmed against the take's notes like an edit
    const TakeSnapshot snap = snapshotOf(take);
    const std::vector<Note> loaded = readNotes(take);
    std::vector<Note> edited = loaded;
    for (Note n : notes) {
        n.sel = false;
        n.idx = n.lyricIdx = -1;
        edited.push_back(n);
    }
    std::vector<int> deleted;
    trimOverlaps(edited, deleted, loaded);
    writeNotes(take, snap, loaded, edited, deleted, velocity);
}
