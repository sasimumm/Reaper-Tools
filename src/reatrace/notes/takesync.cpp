// The active voice's notes <-> its MIDI take over REAPER's MIDI API: loading, committing edits
// as undo points, and the other voices' notes for display.
#include "notes/takesync.h"

#include "index.h"
#include "notes/notesync.h"
#include <array>
#include <span>
#include <sstream>

// Each voice's notes as read from its take, by take index. In the editor a note's idx is its voice's
// offset plus its take index, so the editor's take indices are unique across voices.
static std::array<std::vector<Note>, WINDOW_VOICES> g_loaded;
static std::array<int, WINDOW_VOICES> g_offset;

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

    // The item's position, start offset and playrate, where beat 1 falls (the project tempo, when
    // there are no markers), then every tempo/time signature marker.
    std::string timeMapping() override {
        MediaItem *item = GetMediaItemTake_Item(m_take);
        char buf[160];
        snprintf(buf, sizeof(buf), "%.12g %.12g %.12g;", GetMediaItemInfo_Value(item, "D_POSITION"),
                 GetMediaItemTakeInfo_Value(m_take, "D_STARTOFFS"),
                 GetMediaItemTakeInfo_Value(m_take, "D_PLAYRATE"));
        std::string out = buf;
        snprintf(buf, sizeof(buf), "%.12g;", TimeMap2_QNToTime(NULL, 1));
        out += buf;
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
        if (!MIDI_GetTextSysexEvt(m_take, i, &out->sel, &out->muted, &out->ppq, &out->type,
                                  msg.data(), &size)) {
            return false;
        }
        // terminate within the buffer, whatever size comes back
        msg[idx(std::max(0, std::min(size, static_cast<int>(msg.size()) - 1)))] = 0;
        out->text = msg.data();
        return true;
    }

    void selectNote(int i, bool sel) override {
        const bool noSort = true;
        MIDI_SetNote(m_take, i, &sel, NULL, NULL, NULL, NULL, NULL, NULL, &noSort);
    }

    void deleteNote(int i) override { MIDI_DeleteNote(m_take, i); }

    void insertNote(const NoteEvent &n) override {
        const bool noSort = true;
        MIDI_InsertNote(m_take, n.sel, n.muted, n.start, n.end, n.chan, n.pitch, n.vel, &noSort);
    }

    void deleteText(int i) override { MIDI_DeleteTextSysexEvt(m_take, i); }

    void insertText(const TextEvent &t) override {
        MIDI_InsertTextSysexEvt(m_take, t.sel, t.muted, t.ppq, t.type, t.text.c_str(),
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

// Voice v's notes take, or NULL.
static MediaItem_Take *takeOf(int v) {
    if (v == g_session.voice) {
        return midiTake();
    }
    MediaItem *item = VoiceItem(g_session.audio, v, false);
    return item ? GetActiveTake(item) : NULL;
}

// The voices whose notes go in the editor: the active one, and with * every shown one.
static unsigned editedVoices() {
    const unsigned all = (1u << WINDOW_VOICES) - 1;
    return 1u << g_session.voice | (g_session.allVoices ? all & ~g_ui.hiddenVoices : 0);
}

bool notesStale() {
    if (g_session.editing != editedVoices()) {
        return true;
    }
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if ((g_session.editing >> v & 1) && snapshotOf(takeOf(v)) != g_session.snapshot[idx(v)]) {
            return true;
        }
    }
    return false;
}

// Reads the edited voices' notes into the editor (cancelling any gesture), as the new baseline
// for commitNotes(). The active voice's come last, so they are drawn and hit on top.
void loadNotes() {
    g_session.ed.cancel();
    g_session.ed.notes.clear();
    g_session.ed.deleted.clear();
    g_session.editing = editedVoices();
    int offset = 0;
    for (int k = 1; k <= WINDOW_VOICES; k++) {
        const int v = (g_session.voice + k) % WINDOW_VOICES;
        std::vector<Note> &loaded = g_loaded[idx(v)];
        loaded.clear();
        g_session.snapshot[idx(v)] = {};
        if (!(g_session.editing >> v & 1)) {
            continue;
        }
        MediaItem_Take *take = takeOf(v);
        if (take) {
            ReaperTake t(take);
            loaded = readNotes(t);
        }
        g_session.snapshot[idx(v)] = snapshotOf(take);
        g_offset[idx(v)] = offset;
        for (Note n : loaded) {
            n.idx += offset;
            n.voice = v;
            g_session.ed.notes.push_back(n);
        }
        offset += static_cast<int>(loaded.size());
    }
    g_session.notesVersion++;
    redraw();
}

// Reads the other voices' notes (display only), when their items changed.
void loadOtherVoices(bool force) {
    for (int v = 0; v < WINDOW_VOICES; v++) {
        OtherVoice &o = g_session.others[idx(v)];
        if (g_session.editing >> v & 1) {
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

// Writes the editor's notes back to their voices' takes as one undo point (none for selection-only
// changes). If a take changed since the notes were loaded (an action or script during a drag, an
// item moved, the tempo changed), the edit is dropped and the notes reloaded: the loaded notes
// refer to its events by index and would overwrite the wrong ones. False if it was dropped.
bool commitNotes() {
    if (notesStale()) {
        loadNotes();
        return false;
    }

    // by voice, with take indices back to each voice's own; a note now in another voice than it
    // was read from is deleted there and new in its voice
    const auto origin = [](int i) {
        for (int v = 0; v < WINDOW_VOICES; v++) {
            const int o = g_offset[idx(v)];
            if ((g_session.editing >> v & 1) && i >= o &&
                i < o + static_cast<int>(g_loaded[idx(v)].size())) {
                return v;
            }
        }
        return -1;
    };
    std::array<std::vector<Note>, WINDOW_VOICES> edited;
    std::array<std::vector<int>, WINDOW_VOICES> deleted;
    for (const int i : g_session.ed.deleted) {
        if (const int v = origin(i); v >= 0) {
            deleted[idx(v)].push_back(i - g_offset[idx(v)]);
        }
    }
    for (Note n : g_session.ed.notes) {
        const int v = noteVoice(n), from = n.idx >= 0 ? origin(n.idx) : -1;
        if (from == v) {
            n.idx -= g_offset[idx(v)];
        } else {
            if (from >= 0) {
                deleted[idx(from)].push_back(n.idx - g_offset[idx(from)]);
            }
            n.idx = n.lyricIdx = -1;
        }
        edited[idx(v)].push_back(n);
    }
    std::array<NoteChanges, WINDOW_VOICES> changes;
    bool data = false;
    for (int v = 0; v < WINDOW_VOICES; v++) {
        changes[idx(v)] = diffNotes(g_loaded[idx(v)], edited[idx(v)], deleted[idx(v)]);
        data |= changes[idx(v)].data;
    }
    if (data) {
        Undo_BeginBlock2(NULL);
    }

    bool written = true;
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (!(g_session.editing >> v & 1)) {
            continue;
        }
        // no notes item yet: create one when there is something to write. One may have been made
        // elsewhere since (a script during a drag): then its notes are read, and the editor's (all
        // new) go on top of them, trimmed against them as any edit.
        MediaItem_Take *take = takeOf(v);
        TakeSnapshot loadedFrom = g_session.snapshot[idx(v)];
        std::vector<Note> loaded = g_loaded[idx(v)];
        if (!take && changes[idx(v)].data) {
            MediaItem *found = VoiceItem(g_session.audio, v, false);
            MediaItem *item = found ? found : VoiceItem(g_session.audio, v, true);
            if (v == g_session.voice) {
                g_session.midi = item;
            }
            take = item ? GetActiveTake(item) : NULL;
            if (take) {
                ReaperTake t(take);
                loadedFrom = snapshotOf(t);
                loaded = readNotes(t);
                edited[idx(v)].insert(edited[idx(v)].begin(), loaded.begin(), loaded.end());
            }
        }
        if (!take) {
            written &= !changes[idx(v)].data;
            continue;
        }
        ReaperTake t(take);
        const bool ok = writeNotes(t, loadedFrom, loaded, edited[idx(v)], deleted[idx(v)],
                                   g_session.ed.velocity);
        if (ok && changes[idx(v)].data) {
            fitItemToNotes(take);
        }
        written &= ok;
    }

    if (data) {
        Undo_EndBlock2(NULL, "ReaTrace: edit notes", UNDO_STATE_ITEMS);
        UpdateArrange();
    }
    loadNotes();
    return written;
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

// --- reference tracks ------------------------------------------------------------

static std::string trackGUID(MediaTrack *tr) {
    char buf[64] = "";
    guidToString(GetTrackGUID(tr), buf);
    return buf;
}

static bool hasMIDI(MediaTrack *tr) {
    for (int i = 0; i < CountTrackMediaItems(tr); i++) {
        MediaItem_Take *take = GetActiveTake(GetTrackMediaItem(tr, i));
        if (take && TakeIsMIDI(take)) {
            return true;
        }
    }
    return false;
}

static std::vector<MediaTrack *> voiceTracks();

bool pickReference() {
    const std::vector<MediaTrack *> voices = voiceTracks();
    std::string ref;
    int inside = 0; // folder depth inside a selected folder
    for (int i = 0; i < CountTracks(NULL); i++) {
        MediaTrack *tr = GetTrack(NULL, i);
        const bool selected = GetMediaTrackInfo_Value(tr, "I_SELECTED") != 0;
        if ((inside > 0 || selected) && hasMIDI(tr) &&
            std::find(voices.begin(), voices.end(), tr) == voices.end()) {
            ref += (ref.empty() ? "" : " ") + trackGUID(tr);
        }
        const int depth = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH"));
        inside = inside > 0 ? std::max(0, inside + depth) : (selected && depth > 0 ? depth : 0);
    }
    if (ref.empty()) {
        return false;
    }
    g_settings.reference = ref;
    return true;
}

// The voice tracks of the audio item, which can't be its reference.
static std::vector<MediaTrack *> voiceTracks() {
    std::vector<MediaTrack *> voices;
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (MediaItem *item = VoiceItem(g_session.audio, v, false)) {
            voices.push_back(GetMediaItem_Track(item));
        }
    }
    return voices;
}

bool canReference(MediaTrack *tr) {
    const std::vector<MediaTrack *> voices = voiceTracks();
    return hasMIDI(tr) && std::find(voices.begin(), voices.end(), tr) == voices.end();
}

bool isReference(MediaTrack *tr) {
    const std::string guid = trackGUID(tr);
    std::istringstream guids(g_settings.reference);
    for (std::string g; guids >> g;) {
        if (g == guid) {
            return true;
        }
    }
    return false;
}

void setReference(MediaTrack *tr, bool on) {
    if (isReference(tr) == on) {
        return;
    }
    const std::string guid = trackGUID(tr);
    std::string ref;
    std::istringstream guids(g_settings.reference);
    for (std::string g; guids >> g;) {
        if (g != guid) {
            ref += (ref.empty() ? "" : " ") + g;
        }
    }
    if (on) { // after the others: the voices they suggest for stay put
        ref += (ref.empty() ? "" : " ") + guid;
    }
    g_settings.reference = ref;
    // suggest from the reference while it has tracks, else from the estimator again
    if (!ref.empty()) {
        g_settings.suggestFrom = FROM_REFERENCE;
    } else if (g_settings.suggestFrom == FROM_REFERENCE) {
        g_settings.suggestFrom = FROM_ESTIMATOR;
    }
    loadReference();
}

void loadReference() {
    if (g_settings.suggestFrom != FROM_REFERENCE) {
        return;
    }
    std::vector<Segment> segs;
    std::vector<int> from;
    int tracks = 0;
    const double fr = g_analysis.spec->frameRate();
    const double a = g_session.audioPos, b = a + g_session.audioLen;
    std::istringstream guids(g_settings.reference);
    // a deleted track keeps its place, so the voices after it stay put
    for (std::string guid; guids >> guid; tracks++) {
        MediaTrack *tr = NULL;
        for (int i = 0; i < CountTracks(NULL) && !tr; i++) {
            if (trackGUID(GetTrack(NULL, i)) == guid) {
                tr = GetTrack(NULL, i);
            }
        }
        for (int i = 0; tr && i < CountTrackMediaItems(tr); i++) {
            MediaItem *item = GetTrackMediaItem(tr, i);
            MediaItem_Take *take = GetActiveTake(item);
            if (!take || !TakeIsMIDI(take)) {
                continue;
            }
            // what sounds: the notes within the item, over the audio item
            const double itemPos = GetMediaItemInfo_Value(item, "D_POSITION");
            const double pos = std::max(a, itemPos);
            const double end = std::min(b, itemPos + GetMediaItemInfo_Value(item, "D_LENGTH"));
            ReaperTake t(take);
            for (const Note &n : readNotes(t)) {
                const double s = std::max(n.start, pos), e = std::min(n.end, end);
                if (e > s) {
                    const int f0 = static_cast<int>(lround((s - a) * fr));
                    const int f1 = std::max(f0, static_cast<int>(lround((e - a) * fr)) - 1);
                    segs.push_back({f0, f1, static_cast<double>(n.pitch)});
                    from.push_back(tracks);
                }
            }
        }
    }
    if (segs == g_session.reference && from == g_session.referenceTrack &&
        tracks == g_session.referenceTracks) {
        return;
    }
    g_session.reference = std::move(segs);
    g_session.referenceTrack = std::move(from);
    g_session.referenceTracks = tracks;
    g_session.notesVersion++; // the suggestions changed
    redraw();
}
