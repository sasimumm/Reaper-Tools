#include "isolate.h"

#include <fstream>
#include <sstream>

#include "analysis.h"
#include "commands.h"
#include "guard.h"
#include "state.h"

static const char *const FX_NAME = "JS:ReaTrace/isolate.jsfx";

// Written to REAPER's Effects folder when missing or out of date.
static bool installJSFX() {
    const std::string dir = std::string(GetResourcePath()) + "/Effects/ReaTrace",
                      path = dir + "/isolate.jsfx", want = isolateJSFX();
    std::ifstream in(path);
    std::stringstream have;
    have << in.rdbuf();
    if (have.str() == want) {
        return true;
    }
    RecursiveCreateDirectory(dir.c_str(), 0);
    std::ofstream out(path, std::ios::trunc);
    out << want;
    return (bool)out;
}

static MediaTrack *audioTrack() {
    return ValidatePtr2(NULL, g_session.audio, "MediaItem*") ? GetMediaItem_Track(g_session.audio)
                                                             : NULL;
}

static int findFX(MediaTrack *tr) {
    for (int i = 0; tr && i < TrackFX_GetCount(tr); i++) {
        char name[256] = "";
        TrackFX_GetFXName(tr, i, name, sizeof(name));
        if (strstr(name, ISOLATE_DESC)) {
            return i;
        }
    }
    return -1;
}

static std::vector<double> g_sent; // parameter values last sent, to only send changes

bool isolateOn() { return findFX(audioTrack()) >= 0; }

void setIsolate(bool on) {
    MediaTrack *tr = audioTrack();
    const int fx = findFX(tr);
    g_sent.clear();
    if (!tr || on == (fx >= 0)) {
        return;
    }
    if (!on) {
        TrackFX_Delete(tr, fx);
        return;
    }
    if (!installJSFX() || TrackFX_AddByName(tr, FX_NAME, false, -1) < 0) {
        ReportFailure("isolate", "couldn't add the ReaTrace isolate JSFX to the audio track");
        return;
    }
    updateIsolate();
}

void updateIsolate() {
    MediaTrack *tr = audioTrack();
    const int fx = findFX(tr);
    if (fx < 0) {
        return;
    }
    static_assert(ISOLATE_VOICES == WINDOW_VOICES);
    // what is followed: each voice's notes, and its suggestions where nothing covers them
    const double now = (GetPlayState() & 1) ? GetPlayPosition2() : GetCursorPosition();
    std::vector<Note> spans[WINDOW_VOICES];
    for (int voice = 0; voice < WINDOW_VOICES; voice++) {
        const bool followed = voice == g_session.voice ||
                              (g_settings.isolateAll && !(g_ui.hiddenVoices >> voice & 1));
        if (!followed) {
            continue;
        }
        for (const Note &n :
             voice == g_session.voice ? g_session.ed.notes : g_session.others[voice].notes) {
            if (n.end > now - 1) {
                spans[voice].push_back(n);
            }
        }
    }
    const std::vector<Segment> &sug = suggestions();
    for (size_t i = 0; i < sug.size(); i++) {
        const double a = frameTime(sug[i].f0), b = frameTime(sug[i].f1 + 1);
        const int voice = suggestionVoice(i);
        if (voice >= 0 && b > now - 1 && a < now + 60 &&
            (voice == g_session.voice ||
             (g_settings.isolateAll && !(g_ui.hiddenVoices >> voice & 1))) &&
            !covered(sug[i])) {
            spans[voice].push_back({a, b, (int)lround(sug[i].pitch), false, -1});
        }
    }
    std::vector<double> v((size_t)isolateParam(WINDOW_VOICES));
    v[0] = g_settings.isolateWidth, v[1] = g_settings.isolateBackground;
    for (int voice = 0; voice < WINDOW_VOICES; voice++) {
        const auto sched = pitchSchedule(spans[voice], now - 1, ISOLATE_CHANGES);
        size_t at = (size_t)isolateParam(voice);
        v[at++] = (double)sched.size();
        for (const auto &[t, p] : sched) {
            v[at++] = t, v[at++] = p;
        }
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (i >= g_sent.size() || g_sent[i] != v[i]) {
            TrackFX_SetParam(tr, fx, (int)i, v[i]);
        }
    }
    g_sent = std::move(v);
}
