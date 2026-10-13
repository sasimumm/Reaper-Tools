// The active voice's notes <-> its MIDI take over REAPER's MIDI API: loading, committing edits
// as undo points, and the other voices' notes for display.
#include "notes/takesync.h"

#include "index.h"
#include "notes/notesync.h"
#include <array>
#include <span>

static std::vector<Note> g_loaded; // notes as read from the take, by take index

// --- notes <-> MIDI take ---------------------------------------------------------

// The active voice's notes item's active take, or NULL (also once the item has been deleted).
MediaItem_Take *midiTake() {
    if (g_session.midi && !ValidatePtr2(NULL, g_session.midi, "MediaItem*")) {
        g_session.midi = NULL;
    }
    return g_session.midi ? GetActiveTake(g_session.midi) : NULL;
}

// Take over REAPER's MIDI API.
class ReaperTake : public Take {
  public:
    explicit ReaperTake(MediaItem_Take *take) : m_take(take) {}

    const void *identity() override { return m_take; }

    std::string hash() override {
        char h[128] = "";
        MIDI_GetHash(m_take, false, h, sizeof(h)); // includes text events, so lyric edits count
        return h;
    }

    // The item's position, start offset and playrate, then every tempo/time signature marker.
    std::string timeMapping() override {
        MediaItem *item = GetMediaItemTake_Item(m_take);
        char buf[160];
        snprintf(buf, sizeof(buf), "%.12g %.12g %.12g;", GetMediaItemInfo_Value(item, "D_POSITION"),
                 GetMediaItemTakeInfo_Value(m_take, "D_STARTOFFS"),
                 GetMediaItemTakeInfo_Value(m_take, "D_PLAYRATE"));
        std::string out = buf;
        const int markers = CountTempoTimeSigMarkers(NULL);
        for (int i = 0; i < markers; i++) {
            double time = 0, beat = 0, bpm = 0;
            int measure = 0, num = 0, den = 0;
            bool linear = false;
            GetTempoTimeSigMarker(NULL, i, &time, &measure, &beat, &bpm, &num, &den, &linear);
            snprintf(buf, sizeof(buf), "%.12g %d %.12g %.12g %d/%d %d;", time, measure, beat, bpm,
                     num, den, linear ? 1 : 0);
            out += buf;
        }
        return out;
    }

    double toTime(double ppq) override { return MIDI_GetProjTimeFromPPQPos(m_take, ppq); }
    double toPPQ(double time) override { return MIDI_GetPPQPosFromProjTime(m_take, time); }

    int noteCount() override {
        int notes = 0;
        MIDI_CountEvts(m_take, &notes, NULL, NULL);
        return notes;
    }

    NoteEvent note(int i) override {
        NoteEvent e = {0, 0, 0, false};
        MIDI_GetNote(m_take, i, &e.sel, &e.muted, &e.start, &e.end, &e.chan, &e.pitch, &e.vel);
        return e;
    }

    int textCount() override {
        int texts = 0;
        MIDI_CountEvts(m_take, NULL, NULL, &texts);
        return texts;
    }

    bool text(int i, TextEvent *out) override {
        std::array<char, 1024> msg{};
        int size = static_cast<int>(msg.size()) - 1;
        if (!MIDI_GetTextSysexEvt(m_take, i, NULL, NULL, &out->ppq, &out->type, msg.data(),
                                  &size)) {
            return false;
        }
        // terminate within the buffer, whatever size comes back
        msg[idx(std::max(0, std::min(size, static_cast<int>(msg.size()) - 1)))] = 0;
        out->text = msg.data();
        return true;
    }

    void setNote(int i, const NoteEvent &n) override {
        const bool noSort = true;
        MIDI_SetNote(m_take, i, &n.sel, NULL, &n.start, &n.end, NULL, &n.pitch, NULL, &noSort);
    }

    void deleteNote(int i) override { MIDI_DeleteNote(m_take, i); }

    void insertNote(const NoteEvent &n) override {
        const bool noSort = true;
        MIDI_InsertNote(m_take, n.sel, n.muted, n.start, n.end, n.chan, n.pitch, n.vel, &noSort);
    }

    void deleteText(int i) override { MIDI_DeleteTextSysexEvt(m_take, i); }

    void insertText(const TextEvent &t) override {
        MIDI_InsertTextSysexEvt(m_take, false, false, t.ppq, t.type, t.text.c_str(),
                                static_cast<int>(t.text.size()));
    }

    void sort() override { MIDI_Sort(m_take); }

  private:
    MediaItem_Take *m_take;
};

// The snapshot of a REAPER take; an empty one for NULL.
static TakeSnapshot snapshotOf(MediaItem_Take *take) {
    if (!take) {
        return {};
    }
    ReaperTake t(take);
    return snapshotOf(t);
}

// INT-4: notes drawn, moved or accepted past the item's edges would be hidden and silent, so the
// item grows to cover them (both edges; events keep their project times).
static void fitItemToNotes(MediaItem_Take *take) {
    MediaItem *item = GetMediaItemTake_Item(take);
    const double pos = GetMediaItemInfo_Value(item, "D_POSITION");
    const double end = pos + GetMediaItemInfo_Value(item, "D_LENGTH");

    // the notes' extent, project seconds
    ReaperTake t(take);
    double a = pos, b = end;
    for (int i = 0; i < t.noteCount(); i++) {
        const Take::NoteEvent n = t.note(i);
        a = std::min(a, t.toTime(n.start));
        b = std::max(b, t.toTime(n.end));
    }
    if (a < pos || b > end) {
        MIDI_SetItemExtents(item, TimeMap2_timeToQN(NULL, a), TimeMap2_timeToQN(NULL, b));
    }
}

bool notesStale() { return snapshotOf(midiTake()) != g_session.snapshot; }

// Reads the active take's notes into the editor (cancelling any gesture), as the new baseline
// for commitNotes().
void loadNotes() {
    g_session.ed.cancel();
    MediaItem_Take *take = midiTake();
    if (take) {
        ReaperTake t(take);
        g_loaded = readNotes(t);
    } else {
        g_loaded.clear();
    }
    g_session.ed.notes = g_loaded;
    g_session.ed.deleted.clear();
    g_session.snapshot = snapshotOf(take);
    g_session.notesVersion++;
    redraw();
}

// Reads the other voices' notes (display only), when their items changed.
void loadOtherVoices(bool force) {
    for (int v = 0; v < WINDOW_VOICES; v++) {
        OtherVoice &o = g_session.others[idx(v)];
        if (v == g_session.voice) {
            o = OtherVoice();
            continue;
        }
        MediaItem *item = VoiceItem(g_session.audio, v, false);
        MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
        const TakeSnapshot snap = snapshotOf(take);
        if (!force && item == o.item && snap == o.snapshot) {
            continue;
        }
        o.item = item;
        o.snapshot = snap;
        o.notes.clear();
        if (take) {
            ReaperTake t(take);
            o.notes = readNotes(t);
            for (Note &n : o.notes) {
                n.sel = false;
            }
        }
        g_session.notesVersion++;
        redraw();
    }
}

// Writes the editor's notes back to the take as one undo point (none for selection-only changes).
// If the take changed since the notes were loaded (an action or script during a drag, the item
// moved, the tempo changed), the edit is dropped and the take's notes reloaded: the loaded notes
// refer to its events by index and would overwrite the wrong ones.
void commitNotes() {
    if (notesStale()) {
        loadNotes();
        return;
    }
    const NoteChanges changes = diffNotes(g_loaded, g_session.ed.notes, g_session.ed.deleted);
    if (changes.data) {
        Undo_BeginBlock2(NULL);
    }

    // no notes item yet: create one when there is something to write
    MediaItem_Take *take = midiTake();
    TakeSnapshot loadedFrom = g_session.snapshot;
    if (!take && changes.data) {
        g_session.midi = VoiceItem(g_session.audio, g_session.voice, true);
        if (g_session.midi) {
            take = GetActiveTake(g_session.midi);
            loadedFrom = snapshotOf(take); // new and empty: nothing in it to overwrite
        }
    }

    if (take) {
        ReaperTake t(take);
        if (writeNotes(t, loadedFrom, g_loaded, g_session.ed.notes, g_session.ed.deleted,
                       g_session.ed.velocity) &&
            changes.data) {
            fitItemToNotes(take);
        }
    }

    if (changes.data) {
        Undo_EndBlock2(NULL, "ReaTrace: edit notes", UNDO_STATE_ITEMS);
        UpdateArrange();
    }
    loadNotes();
}

// Notes straight into another voice's take (its track and item are created if needed).
void insertIntoVoice(int v, std::span<const Note> notes) {
    MediaItem *item = VoiceItem(g_session.audio, v, true);
    MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
    if (take) {
        ReaperTake t(take);
        appendNotes(t, notes, g_session.ed.velocity);
        fitItemToNotes(take);
    }
}
