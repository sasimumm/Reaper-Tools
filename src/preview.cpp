#include "preview.h"

#include <fcntl.h>
#include <unistd.h>

// --- note preview ----------------------------------------------------------------------

// Plays a short note through the notes track (so through its synth) as a track preview, from a
// tiny generated MIDI file per pitch; nothing needs to be armed. Previews always play to the end:
// stopping one mid-note leaves the synth without a note-off, so a pool lets them overlap.
// Seconds (at normal speed) before a finished preview is stopped. The file is ~1 s at 120 bpm;
// generous in case REAPER plays it at a slower project tempo, since stopping before the
// note-off hangs the note.
static const double PREVIEW_LIFE = 2.5;
static const int PREVIEW_SLOTS = 16;
struct Preview {
    preview_register_t reg;
    bool init, playing;
    double started;
};
static Preview g_previews[PREVIEW_SLOTS];

static void stopPreview(Preview &p) {
    if (p.playing) {
        StopTrackPreview2(NULL, &p.reg);
    }
    p.playing = false;
    delete p.reg.src;
    p.reg.src = NULL;
}

void stopPreviews(bool onlyFinished) {
    const double now = time_precise();
    // previews follow the master playrate, so at 50% speed they take twice as long
    const double life = PREVIEW_LIFE / std::max(0.1, Master_GetPlayRate(NULL));
    for (Preview &p : g_previews) {
        if (p.playing && (!onlyFinished || now - p.started > life)) {
            stopPreview(p);
        }
    }
}

void previewPitch(int pitch) {
    if (!g_settings.audition || pitch < 0 || pitch > 127 || !g_session.midi ||
        !ValidatePtr2(NULL, g_session.midi, "MediaItem*")) {
        return;
    }
    stopPreviews(true);
    Preview *slot = NULL;
    for (Preview &p : g_previews) {
        if (!p.playing) {
            slot = &p;
            break;
        }
    }
    if (!slot) {
        return; // all busy: skip rather than cut a note off
    }

    // Written fresh every time into REAPER's per-user resource folder, never following a symlink.
    // One file per pitch, in case REAPER caches MIDI file sources by path; files of playing
    // previews are only rewritten with identical bytes.
    char path[2048];
    snprintf(path, sizeof(path), "%s/Data/reatrace-preview-%d.mid", GetResourcePath(), pitch);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    // format 0, 96 ticks per quarter: note on; note off after 77 ticks (~0.4 s at 120 bpm); all notes
    // off; end of track 96 ticks later, so the note-off isn't on the source's last tick (where it
    // was not played, leaving the note hanging)
    // clang-format off: one MIDI event per line
    const unsigned char smf[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 16,
                                 0, 0x90, (unsigned char)pitch, 96,
                                 77, 0x80, (unsigned char)pitch, 0,
                                 10, 0xb0, 0x7b, 0,
                                 96, 0xff, 0x2f, 0};
    // clang-format on
    const bool ok = write(fd, smf, sizeof(smf)) == (ssize_t)sizeof(smf);
    close(fd);
    if (!ok) {
        return;
    }
    if (!slot->init) {
        pthread_mutex_init(&slot->reg.mutex, NULL), slot->init = true;
    }
    slot->reg.src = PCM_Source_CreateFromFile(path);
    if (!slot->reg.src) {
        return;
    }
    slot->reg.m_out_chan = -1; // -1: play through preview_track
    slot->reg.preview_track = GetMediaItem_Track(g_session.midi);
    slot->reg.curpos = 0, slot->reg.loop = false, slot->reg.volume = 1;
    slot->playing = PlayTrackPreview2Ex(NULL, &slot->reg, 0, 0) != 0;
    slot->started = time_precise();
    if (!slot->playing) {
        stopPreview(*slot);
    }
}

static int g_lastDragPitch = -1;

void previewDragStarted(int pitch) { g_lastDragPitch = pitch; }

// While drawing or moving, play the dragged note again whenever its pitch changes.
void previewDragged() {
    const int dn = g_session.ed.dragNote();
    const int pitch = dn >= 0 ? g_session.ed.notes[dn].pitch : -1;
    if (pitch >= 0 && pitch != g_lastDragPitch) {
        previewPitch(pitch);
    }
    g_lastDragPitch = pitch;
}
