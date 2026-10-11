#include "ui.h"

static std::vector<Note> g_loaded; // notes as read from the take, by take index

// --- notes <-> MIDI take ---------------------------------------------------------

MediaItem_Take *midiTake() {
    if (g_midi && !ValidatePtr2(NULL, g_midi, "MediaItem*")) {
        g_midi = NULL;
    }
    return g_midi ? GetActiveTake(g_midi) : NULL;
}

std::string takeHash(MediaItem_Take *take) {
    char h[128] = "";
    if (take) {
        MIDI_GetHash(take, false, h, sizeof(h)); // includes text events, so lyric edits count
    }
    return h;
}

static const int LYRIC_EVENT = 5; // MIDI meta event type used by REAPER's notation lyrics

// Lyrics are MIDI lyric events at a note's start; each attaches to the first note starting there.
void loadNotes() {
    g_ed.cancel();
    g_loaded.clear();
    MediaItem_Take *take = midiTake();
    int cnt = 0, textCnt = 0;
    if (take) {
        MIDI_CountEvts(take, &cnt, NULL, &textCnt);
    }
    std::vector<double> startPPQ;
    for (int i = 0; i < cnt; i++) {
        bool sel = false;
        double s = 0, e = 0;
        int pitch = 0;
        MIDI_GetNote(take, i, &sel, NULL, &s, &e, NULL, &pitch, NULL);
        g_loaded.push_back(
            {MIDI_GetProjTimeFromPPQPos(take, s), MIDI_GetProjTimeFromPPQPos(take, e), pitch, sel,
             i});
        startPPQ.push_back(s);
    }
    for (int i = 0; i < textCnt; i++) {
        char msg[1024];
        int sz = sizeof(msg) - 1, type = 0;
        double ppq = 0;
        if (!MIDI_GetTextSysexEvt(take, i, NULL, NULL, &ppq, &type, msg, &sz) ||
            type != LYRIC_EVENT) {
            continue;
        }
        msg[std::max(0, std::min(sz, (int)sizeof(msg) - 1))] = 0;
        for (size_t n = 0; n < g_loaded.size(); n++) {
            if (g_loaded[n].lyricIdx < 0 && fabs(startPPQ[n] - ppq) < 0.5) {
                g_loaded[n].lyric = msg, g_loaded[n].lyricIdx = i;
                break;
            }
        }
    }
    g_ed.notes = g_loaded;
    g_ed.deleted.clear();
    g_hash = takeHash(take);
    g_notesVersion++;
    redraw();
}

// Reads the other voices' notes (display only), when their items changed.
void loadOtherVoices(bool force) {
    for (int v = 0; v < WINDOW_VOICES; v++) {
        OtherVoice &o = g_others[v];
        if (v == g_voice) {
            o = OtherVoice();
            continue;
        }
        MediaItem *item = VoiceItem(g_audio, v, false);
        MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
        const std::string h = takeHash(take);
        if (!force && item == o.item && h == o.hash) {
            continue;
        }
        o.item = item, o.hash = h, o.notes.clear();
        int cnt = 0;
        if (take) {
            MIDI_CountEvts(take, &cnt, NULL, NULL);
        }
        for (int i = 0; i < cnt; i++) {
            double st = 0, en = 0;
            int pitch = 0;
            MIDI_GetNote(take, i, NULL, NULL, &st, &en, NULL, &pitch, NULL);
            o.notes.push_back(
                {MIDI_GetProjTimeFromPPQPos(take, st), MIDI_GetProjTimeFromPPQPos(take, en), pitch,
                 false, i});
        }
        g_notesVersion++;
        redraw();
    }
}

// Writes the editor's notes back to the take as one undo point (none for selection-only changes).
void commitNotes() {
    bool dataChanged = !g_ed.deleted.empty(), lyricsChanged = false;
    for (int i : g_ed.deleted) {
        lyricsChanged |= g_loaded[i].lyricIdx >= 0;
    }
    for (const Note &n : g_ed.notes) {
        if (n.idx < 0 || n.idx >= (int)g_loaded.size()) {
            dataChanged = true, lyricsChanged |= !n.lyric.empty();
            continue;
        }
        const Note &o = g_loaded[n.idx];
        dataChanged |=
            n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.lyric != o.lyric;
        lyricsChanged |= n.lyric != o.lyric || (n.start != o.start && !n.lyric.empty());
    }
    if (dataChanged) {
        Undo_BeginBlock2(NULL);
    }
    MediaItem_Take *take = midiTake();
    if (!take && dataChanged && (g_midi = VoiceItem(g_audio, g_voice, true))) {
        take = GetActiveTake(g_midi);
    }
    if (take) {
        const bool noSort = true;
        for (const Note &n : g_ed.notes) {
            if (n.idx < 0 || n.idx >= (int)g_loaded.size()) {
                continue;
            }
            const Note &o = g_loaded[n.idx];
            if (n.start == o.start && n.end == o.end && n.pitch == o.pitch && n.sel == o.sel) {
                continue;
            }
            const double s = MIDI_GetPPQPosFromProjTime(take, n.start),
                         e = MIDI_GetPPQPosFromProjTime(take, n.end);
            MIDI_SetNote(take, n.idx, &n.sel, NULL, &s, &e, NULL, &n.pitch, NULL, &noSort);
        }
        std::vector<int> del = g_ed.deleted;
        std::sort(del.rbegin(), del.rend()); // highest first so earlier indices stay valid
        for (int i : del) {
            MIDI_DeleteNote(take, i);
        }
        for (const Note &n : g_ed.notes) {
            if (n.idx < 0) {
                MIDI_InsertNote(take, n.sel, false, MIDI_GetPPQPosFromProjTime(take, n.start),
                                MIDI_GetPPQPosFromProjTime(take, n.end), 0, n.pitch, g_ed.velocity,
                                &noSort);
            }
        }
        if (lyricsChanged) {
            // rewrite the lyric events attached to notes; unattached text events are left alone
            std::vector<int> old;
            for (const Note &o : g_loaded) {
                if (o.lyricIdx >= 0) {
                    old.push_back(o.lyricIdx);
                }
            }
            std::sort(old.rbegin(), old.rend());
            for (int i : old) {
                MIDI_DeleteTextSysexEvt(take, i);
            }
            for (const Note &n : g_ed.notes) {
                if (!n.lyric.empty()) {
                    MIDI_InsertTextSysexEvt(take, false, false,
                                            MIDI_GetPPQPosFromProjTime(take, n.start), LYRIC_EVENT,
                                            n.lyric.c_str(), (int)n.lyric.size());
                }
            }
        }
        MIDI_Sort(take);
    }
    if (dataChanged) {
        Undo_EndBlock2(NULL, "ReaTrace: edit notes", UNDO_STATE_ITEMS);
        UpdateArrange();
    }
    loadNotes();
}

// Notes straight into another voice's take (its track and item are created if needed).
void insertIntoVoice(int v, const std::vector<Note> &notes) {
    MediaItem *item = VoiceItem(g_audio, v, true);
    MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
    if (!take) {
        return;
    }
    const bool noSort = true;
    for (const Note &n : notes) {
        MIDI_InsertNote(take, false, false, MIDI_GetPPQPosFromProjTime(take, n.start),
                        MIDI_GetPPQPosFromProjTime(take, n.end), 0, n.pitch, g_ed.velocity,
                        &noSort);
    }
    MIDI_Sort(take);
}
