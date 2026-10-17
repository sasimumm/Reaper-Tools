// Reading the editor's notes from a Take and writing edits back, independent of REAPER.
#include "notes/notesync.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <span>
#include <tuple>

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

bool trimOverlaps(std::vector<Note> &notes, std::vector<int> &deleted) {
    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Note &x = notes[a], &y = notes[b];
        return std::tie(x.chan, x.pitch, x.start) < std::tie(y.chan, y.pitch, y.start);
    });
    // Each note is cut at the next one's start, so every earlier note ends by the current one's
    // start and only the last kept note can overlap it.
    std::vector<bool> drop(notes.size());
    bool any = false;
    size_t prev = SIZE_MAX;
    for (const size_t k : order) {
        const Note &b = notes[k];
        if (prev != SIZE_MAX) {
            Note &a = notes[prev];
            if (a.chan == b.chan && a.pitch == b.pitch && a.start == b.start) {
                // a duplicate: one note, as long as the longer, keeping a lyric and selection
                a.end = std::max(a.end, b.end);
                if (a.lyric.empty()) {
                    a.lyric = b.lyric;
                }
                a.sel |= b.sel;
                drop[k] = any = true;
                continue;
            }
            if (a.chan == b.chan && a.pitch == b.pitch && a.end > b.start) {
                a.end = b.start;
                any = true;
            }
        }
        prev = k;
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
    // edited notes' lyrics by pitch, with their starts in PPQ, to find them from the take's notes:
    // the take keeps whole ticks, rounded its own way, so a start matches within a tick
    struct Lyric {
        double start;
        const std::string *text;
        bool used = false;
    };
    std::map<int, std::vector<Lyric>> lyricOf;
    for (const Note &n : edited) {
        if (!n.lyric.empty()) {
            lyricOf[n.pitch].push_back({take.toPPQ(n.start), &n.lyric});
        }
    }
    // the closest unused lyric of this pitch within a tick of start
    const auto find = [&](double start, int pitch) -> const std::string * {
        Lyric *best = nullptr;
        for (Lyric &l : lyricOf[pitch]) {
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
            group.push_back({e.start, find(e.start, e.pitch)});
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
    std::vector<int> del(deleted.begin(), deleted.end());
    std::sort(del.rbegin(), del.rend());
    for (int i : del) {
        take.deleteNote(i);
    }
    std::sort(del.begin(), del.end());

    // then changed notes in place (their indices less the deleted notes before them), and new
    // notes, unsorted until sort()
    for (const Note &n : edited) {
        if (!known(n, loaded)) {
            continue;
        }
        const Note &o = loaded[static_cast<size_t>(n.idx)];
        if (n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.sel != o.sel) {
            const auto before = std::lower_bound(del.begin(), del.end(), n.idx) - del.begin();
            take.setNote(n.idx - static_cast<int>(before), toEvent(take, n, n.sel, velocity));
        }
    }
    for (const Note &n : edited) {
        if (n.idx < 0) {
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
    trimOverlaps(edited, deleted);
    writeNotes(take, snap, loaded, edited, deleted, velocity);
}
