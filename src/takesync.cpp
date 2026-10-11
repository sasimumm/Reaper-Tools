#include "ui.h"

#include "notesync.h"

static std::vector<Note> g_loaded; // notes as read from the take, by take index

// --- notes <-> MIDI take ---------------------------------------------------------

MediaItem_Take *midiTake() {
    if (g_session.midi && !ValidatePtr2(NULL, g_session.midi, "MediaItem*")) {
        g_session.midi = NULL;
    }
    return g_session.midi ? GetActiveTake(g_session.midi) : NULL;
}

std::string takeHash(MediaItem_Take *take) {
    char h[128] = "";
    if (take) {
        MIDI_GetHash(take, false, h, sizeof(h)); // includes text events, so lyric edits count
    }
    return h;
}

// Take over REAPER's MIDI API.
class ReaperTake : public Take {
  public:
    explicit ReaperTake(MediaItem_Take *take) : m_take(take) {}
    double toTime(double ppq) override { return MIDI_GetProjTimeFromPPQPos(m_take, ppq); }
    double toPPQ(double time) override { return MIDI_GetPPQPosFromProjTime(m_take, time); }
    int noteCount() override {
        int notes = 0;
        MIDI_CountEvts(m_take, &notes, NULL, NULL);
        return notes;
    }
    NoteEvent note(int i) override {
        NoteEvent e = {0, 0, 0, false};
        MIDI_GetNote(m_take, i, &e.sel, NULL, &e.start, &e.end, NULL, &e.pitch, NULL);
        return e;
    }
    int textCount() override {
        int texts = 0;
        MIDI_CountEvts(m_take, NULL, NULL, &texts);
        return texts;
    }
    bool text(int i, TextEvent *out) override {
        char msg[1024];
        int size = sizeof(msg) - 1;
        if (!MIDI_GetTextSysexEvt(m_take, i, NULL, NULL, &out->ppq, &out->type, msg, &size)) {
            return false;
        }
        msg[std::max(0, std::min(size, (int)sizeof(msg) - 1))] = 0;
        out->text = msg;
        return true;
    }
    void setNote(int i, const NoteEvent &n) override {
        const bool noSort = true;
        MIDI_SetNote(m_take, i, &n.sel, NULL, &n.start, &n.end, NULL, &n.pitch, NULL, &noSort);
    }
    void deleteNote(int i) override { MIDI_DeleteNote(m_take, i); }
    void insertNote(const NoteEvent &n, int velocity) override {
        const bool noSort = true;
        MIDI_InsertNote(m_take, n.sel, false, n.start, n.end, 0, n.pitch, velocity, &noSort);
    }
    void deleteText(int i) override { MIDI_DeleteTextSysexEvt(m_take, i); }
    void insertText(const TextEvent &t) override {
        MIDI_InsertTextSysexEvt(m_take, false, false, t.ppq, t.type, t.text.c_str(),
                                (int)t.text.size());
    }
    void sort() override { MIDI_Sort(m_take); }

  private:
    MediaItem_Take *m_take;
};

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
    g_session.hash = takeHash(take);
    g_session.notesVersion++;
    redraw();
}

// Reads the other voices' notes (display only), when their items changed.
void loadOtherVoices(bool force) {
    for (int v = 0; v < WINDOW_VOICES; v++) {
        OtherVoice &o = g_session.others[v];
        if (v == g_session.voice) {
            o = OtherVoice();
            continue;
        }
        MediaItem *item = VoiceItem(g_session.audio, v, false);
        MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
        const std::string h = takeHash(take);
        if (!force && item == o.item && h == o.hash) {
            continue;
        }
        o.item = item, o.hash = h, o.notes.clear();
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
void commitNotes() {
    const NoteChanges changes = diffNotes(g_loaded, g_session.ed.notes, g_session.ed.deleted);
    if (changes.data) {
        Undo_BeginBlock2(NULL);
    }
    MediaItem_Take *take = midiTake();
    if (!take && changes.data &&
        (g_session.midi = VoiceItem(g_session.audio, g_session.voice, true))) {
        take = GetActiveTake(g_session.midi);
    }
    if (take) {
        ReaperTake t(take);
        writeNotes(t, g_loaded, g_session.ed.notes, g_session.ed.deleted, g_session.ed.velocity);
    }
    if (changes.data) {
        Undo_EndBlock2(NULL, "ReaTrace: edit notes", UNDO_STATE_ITEMS);
        UpdateArrange();
    }
    loadNotes();
}

// Notes straight into another voice's take (its track and item are created if needed).
void insertIntoVoice(int v, const std::vector<Note> &notes) {
    MediaItem *item = VoiceItem(g_session.audio, v, true);
    MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
    if (take) {
        ReaperTake t(take);
        appendNotes(t, notes, g_session.ed.velocity);
    }
}
